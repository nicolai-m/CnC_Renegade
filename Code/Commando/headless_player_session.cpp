// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_player_session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "connect.h"
#include "control.h"
#include "headless_collision.h"
#include "headless_join_data.h"
#include "humanstate.h"
#include "netclassids.h"
#include "networkobject.h"

namespace {

using namespace std::chrono_literals;
constexpr auto InputTimeout = 250ms;
constexpr auto PublishInterval = 50ms;
constexpr float SoldierEyeHeight = 1.62f;
// NetworkObjectMgrClass reserves 100,000 client object IDs per peer.
constexpr int ClientObjectBase = 2110000001;
constexpr int ClientObjectCount = 100000;
constexpr UINT ObjectHeaderBits = 32 + 8 + 1;
constexpr UINT HumanStateBits = 5;
constexpr UINT HumanSubStateBits = 9;
constexpr double PositionResolution = 0.2;

Vector2 Left_Of(const Vector2 &forward)
{
    return Vector2(-forward.Y, forward.X);
}

} // namespace

HeadlessPlayerSession::HeadlessPlayerSession(const HeadlessCollision &collision, cConnection &connection)
    : Simulation(collision), Connection(connection)
{
    AnalogEncoder.Init(-1.0, 1.0, 0.01); // ControlClass::Set_Precision
    Init_Position_Encoders(collision, PositionEncoders);
}

void HeadlessPlayerSession::Validate_Map(const HeadlessCollision &collision)
{
    std::array<cEncoderTypeEntry, 3> encoders;
    Init_Position_Encoders(collision, encoders);
}

void HeadlessPlayerSession::Init_Position_Encoders(const HeadlessCollision &collision,
    std::array<cEncoderTypeEntry, 3> &encoders)
{
    const auto &bounds = collision.Level().World_Bounds();
    const auto minimum = bounds.Center - bounds.Extent;
    const auto maximum = bounds.Center + bounds.Extent;
    for (std::size_t axis = 0; axis < encoders.size(); ++axis) {
        const double min = static_cast<double>(minimum[axis]) - 1;
        const double max = static_cast<double>(maximum[axis]) + 1;
        // CombatGameModeClass::Compute_World_Size uses 32-bit position codes at most.
        if (!std::isfinite(min) || !std::isfinite(max) ||
            max - min > PositionResolution * std::numeric_limits<ULONG>::max()) {
            throw std::runtime_error("Map extents exceed the network position range");
        }
        encoders[axis].Invalidate();
        encoders[axis].Init(min, max, PositionResolution);
    }
}

bool HeadlessPlayerSession::Attach_Player(int peer_id, int player_type, int soldier_object_id)
{
    if (peer_id <= SERVER_RHOST_ID || peer_id > Connection.Get_Max_RHost() ||
        !Connection.Get_Remote_Host(peer_id) || soldier_object_id <= 0 ||
        std::any_of(Peers.begin(), Peers.end(), [=](const auto &entry) {
            return entry.second.SoldierObject == soldier_object_id;
        }) || !Simulation.Spawn_Soldier(peer_id, player_type)) {
        return false;
    }
    const auto &soldier = *Simulation.Find_Soldier(peer_id);
    const Vector2 forward(std::cos(soldier.SpawnHeading), std::sin(soldier.SpawnHeading));
    const auto target = soldier.Position + Vector3(forward.X, forward.Y, SoldierEyeHeight);
    const auto previous = Peers.find(peer_id);
    const int control = previous == Peers.end() ? 0 : previous->second.ControlObject;
    auto &peer = Peers.insert_or_assign(peer_id, Peer{soldier_object_id, forward, target, control}).first->second;
    peer.Viewers.insert(peer_id);
    return true;
}

void HeadlessPlayerSession::Introduce_Soldier(int owner_id, int recipient, const HeadlessSoldierPreset &preset,
    unsigned armor_types)
{
    const auto &soldier = *Simulation.Find_Soldier(owner_id);
    auto &peer = Peers.at(owner_id);
    cPacket packet;
    packet.Add(peer.SoldierObject);
    packet.Add(static_cast<BYTE>(NetworkObjectClass::BIT_CREATION));
    packet.Add(false);
    packet.Add(NETCLASSID_GAMEOBJ);
    packet.Add(preset.DefinitionId); // NetworkGameObjectFactoryClass::Prep_Packet.
    Write_Position(packet, soldier.Position);
    packet.Add(soldier.SpawnHeading);
    packet.Add(owner_id); // SmartGameObj::Export_Creation.

    packet.Add_Terminated_String(preset.Model.c_str());
    packet.Add_Terminated_String("", true); // Animation.
    packet.Add(0); // Current frame.
    packet.Add(0); // Target frame.
    packet.Add(3); // ANIM_MODE_TARGET.
    packet.Add(0); // Host object.
    packet.Add(0); // Host bone.
    packet.Add(soldier.PlayerType);
    packet.Add(false); // HUD poke indicator.
    packet.Add(preset.DefinitionId); // SoldierGameObj::Export_Rare.

    packet.Add(false); // Health is nonzero.
    cEncoderTypeEntry vitals;
    vitals.Init(0, 2000, 1);
    cEncoderTypeEntry armor;
    armor.Init(0, armor_types, 1);
    ULONG code;
    vitals.Scale(preset.Health, code);
    packet.Add_Bits(code, vitals.Get_Bit_Precision());
    vitals.Scale(preset.Shield, code);
    packet.Add_Bits(code, vitals.Get_Bit_Precision());
    armor.Scale(preset.ShieldType, code);
    packet.Add_Bits(code, armor.Get_Bit_Precision());
    packet.Add(0); // Weapon list size.
    Write_Frequent(packet, owner_id, peer);
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
    peer.Viewers.insert(recipient);
}

void HeadlessPlayerSession::Set_Player_Loading(int peer_id, bool loading)
{
    if (const auto found = Peers.find(peer_id); found != Peers.end()) {
        auto &peer = found->second;
        peer.Ready = !loading;
        if (loading) {
            peer.MoveAxes.Set(0, 0);
            peer.InputRemaining = {};
            Simulation.Set_Move_Input(peer_id, peer.MoveAxes);
        }
    }
}

void HeadlessPlayerSession::Remove_Player(int peer_id)
{
    Peers.erase(peer_id);
    Simulation.Remove_Soldier(peer_id);
    for (auto &[id, peer] : Peers) {
        peer.Viewers.erase(peer_id);
    }
}

void HeadlessPlayerSession::Reset_Map(const HeadlessCollision &collision)
{
    Init_Position_Encoders(collision, PositionEncoders);
    Simulation = HeadlessSimulation(collision);
    PublishElapsed = {};
    for (auto &[id, peer] : Peers) {
        peer.Ready = false;
        peer.SoldierObject = 0;
        peer.MoveAxes.Set(0, 0);
        peer.InputRemaining = {};
        peer.Viewers.clear();
    }
}

void HeadlessPlayerSession::Receive_Control(cPacket &packet, int peer_id)
{
    const auto peer = Peers.find(peer_id);
    if (peer != Peers.end() && peer->second.Ready) {
        Read_Control(packet, peer_id, peer->second);
    }
    packet.Flush();
}

bool HeadlessPlayerSession::Read_Control(cPacket &packet, int peer_id, Peer &peer)
{
    if (packet.Get_Bits_Remaining() < ObjectHeaderBits) {
        return false;
    }
    int object_id;
    BYTE dirty;
    bool deleting;
    packet.Get(object_id);
    packet.Get(dirty);
    packet.Get(deleting);

    const bool creating = dirty == NetworkObjectClass::BIT_CREATION;
    if (deleting || !Client_Owns_Object_Id(peer_id, object_id) ||
        (creating ? (peer.ControlObject != 0 || packet.Get_Type() != PACKETTYPE_RELIABLE) :
            (dirty != NetworkObjectClass::BIT_FREQUENT || object_id != peer.ControlObject))) {
        return false;
    }
    if (creating) {
        if (packet.Get_Bits_Remaining() < 64) {
            return false;
        }
        int class_id;
        int owner;
        if (packet.Get(class_id) != NETCLASSID_CLIENTCONTROL || packet.Get(owner) != peer_id) {
            return false;
        }
    }

    if (packet.Get_Bits_Remaining() < 32) {
        return false;
    }
    int soldier_id;
    packet.Get(soldier_id);
    if (soldier_id == -1 && packet.Is_Flushed()) {
        // The original control object can arrive before its first movement sample.
        peer.ControlObject = object_id;
        return true;
    }
    if (soldier_id != peer.SoldierObject) {
        return false;
    }

    constexpr UINT control_flags = ControlClass::NUM_BOOLEAN_ONE_TIME + ControlClass::NUM_BOOLEAN_CONTINUOUS;
    const UINT control_bits = control_flags + ControlClass::ANALOG_CONTROL_COUNT * AnalogEncoder.Get_Bit_Precision();
    const UINT target_bits = PositionEncoders[0].Get_Bit_Precision() + PositionEncoders[1].Get_Bit_Precision() +
        PositionEncoders[2].Get_Bit_Precision();
    if (packet.Get_Bits_Remaining() < control_bits + 2 + target_bits) {
        return false;
    }
    ULONG bits;
    packet.Get_Bits(bits, control_flags);
    std::array<float, ControlClass::ANALOG_CONTROL_COUNT> analog;
    for (auto &value : analog) {
        packet.Get_Bits(bits, AnalogEncoder.Get_Bit_Precision());
        value = static_cast<float>(AnalogEncoder.Unscale(bits));
        if (std::abs(value) < 0.005f) {
            value = 0;
        }
    }
    packet.Get_Bits(bits, 1); // Sniping toggle.
    bool checking;
    packet.Get(checking);
    if (packet.Get_Bits_Remaining() != target_bits + (checking ? 32 : 0)) {
        return false;
    }
    if (checking) {
        packet.Get_Bits(bits, 32); // The original soldier packet can carry a checksum.
    }
    Vector3 relative_target;
    for (std::size_t axis = 0; axis < PositionEncoders.size(); ++axis) {
        auto &encoder = PositionEncoders[axis];
        packet.Get_Bits(bits, encoder.Get_Bit_Precision());
        relative_target[axis] = static_cast<float>(encoder.Unscale(bits));
    }

    Vector2 forward(relative_target.X, relative_target.Y);
    if (forward.Length2() > 0.01f) {
        forward.Normalize();
        peer.Forward = forward;
    }
    peer.Target = Simulation.Find_Soldier(peer_id)->Position + relative_target;
    peer.MoveAxes.Set(analog[ControlClass::ANALOG_MOVE_FORWARD], analog[ControlClass::ANALOG_MOVE_LEFT]);
    const auto world_move = peer.MoveAxes.X * peer.Forward + peer.MoveAxes.Y * Left_Of(peer.Forward);
    // Rotation and floating-point rounding can put one component just outside the input range.
    Simulation.Set_Move_Input(peer_id, Vector2(std::clamp(world_move.X, -1.0f, 1.0f),
        std::clamp(world_move.Y, -1.0f, 1.0f)));
    peer.InputRemaining = InputTimeout;
    peer.ControlObject = object_id;
    return true;
}

bool HeadlessPlayerSession::Client_Owns_Object_Id(int peer_id, int object_id)
{
    if (peer_id <= SERVER_RHOST_ID || peer_id > cConnection::MAX_PEER_ID) {
        return false;
    }
    const auto first = ClientObjectBase + (peer_id - 1) * ClientObjectCount;
    return object_id >= first && object_id < first + ClientObjectCount;
}

void HeadlessPlayerSession::Advance(HeadlessSimulation::Clock::duration elapsed)
{
    using Duration = HeadlessSimulation::Clock::duration;
    if (elapsed <= Duration::zero()) {
        return;
    }
    for (auto &[id, peer] : Peers) {
        if (peer.InputRemaining > Duration::zero()) {
            peer.InputRemaining -= std::min(elapsed, peer.InputRemaining);
            if (peer.InputRemaining == Duration::zero()) {
                peer.MoveAxes.Set(0, 0);
                Simulation.Set_Move_Input(id, peer.MoveAxes);
            }
        }
    }
    Simulation.Advance(elapsed);
    PublishElapsed += std::min(elapsed, std::chrono::duration_cast<Duration>(PublishInterval));
    if (PublishElapsed >= PublishInterval) {
        PublishElapsed -= PublishInterval;
        for (const auto &[id, peer] : Peers) {
            Publish_Player(id, peer);
        }
    }
}

void HeadlessPlayerSession::Write_Position(cPacket &packet, const Vector3 &position)
{
    for (std::size_t axis = 0; axis < PositionEncoders.size(); ++axis) {
        auto &encoder = PositionEncoders[axis];
        ULONG encoded;
        encoder.Scale(position[axis], encoded);
        packet.Add_Bits(encoded, encoder.Get_Bit_Precision());
    }
}

void HeadlessPlayerSession::Write_Frequent(cPacket &packet, int peer_id, const Peer &peer)
{
    const auto &soldier = *Simulation.Find_Soldier(peer_id);
    packet.Add(false); // In vehicle.
    packet.Add(false); // Holding a weapon.
    Write_Position(packet, soldier.Position);
    packet.Add_Bits(soldier.Grounded ? HumanStateClass::UPRIGHT : HumanStateClass::AIRBORNE, HumanStateBits);
    const auto left = Left_Of(peer.Forward);
    const float forward_speed = soldier.Velocity.X * peer.Forward.X + soldier.Velocity.Y * peer.Forward.Y;
    const float left_speed = soldier.Velocity.X * left.X + soldier.Velocity.Y * left.Y;
    ULONG stance = 0;
    if (forward_speed > 0.01f) { stance |= HumanStateClass::SUB_STATE_FORWARD; }
    if (forward_speed < -0.01f) { stance |= HumanStateClass::SUB_STATE_BACKWARD; }
    if (left_speed > 0.01f) { stance |= HumanStateClass::SUB_STATE_LEFT; }
    if (left_speed < -0.01f) { stance |= HumanStateClass::SUB_STATE_RIGHT; }
    packet.Add_Bits(stance, HumanSubStateBits);
    if (!soldier.Grounded) {
        packet.Add(soldier.Velocity.X);
        packet.Add(soldier.Velocity.Y);
        packet.Add(soldier.Velocity.Z);
    }
    packet.Add(false); // Special damage.
    packet.Add(false); // Attached to a host bone.
    Write_Position(packet, peer.Target);
    packet.Add_Bits(0, ControlClass::NUM_BOOLEAN_CONTINUOUS);
    const float analog[] = {peer.MoveAxes.X, peer.MoveAxes.Y, 0, 0};
    for (const auto value : analog) {
        ULONG bits;
        AnalogEncoder.Scale(value, bits);
        packet.Add_Bits(bits, AnalogEncoder.Get_Bit_Precision());
    }
}

void HeadlessPlayerSession::Publish_Player(int peer_id, const Peer &peer)
{
    if (!peer.Ready) {
        return;
    }
    cPacket packet;
    packet.Add(peer.SoldierObject);
    packet.Add(static_cast<BYTE>(NetworkObjectClass::BIT_FREQUENT));
    packet.Add(false);
    Write_Frequent(packet, peer_id, peer);
    for (const int recipient : peer.Viewers) {
        const auto viewer = Peers.find(recipient);
        if (viewer != Peers.end() && viewer->second.Ready && Connection.Get_Remote_Host(recipient)) {
            Connection.Send_Packet_To_Individual(packet, recipient, SEND_UNRELIABLE);
        }
    }
}
