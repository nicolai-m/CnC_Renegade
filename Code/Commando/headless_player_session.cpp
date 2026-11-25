// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_player_session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "aabox.h"
#include "buildingstate.h"
#include "colmath.h"
#include "coltype.h"
#include "connect.h"
#include "control.h"
#include "headless_collision.h"
#include "headless_join_data.h"
#include "humanstate.h"
#include "lineseg.h"
#include "netclassids.h"
#include "networkobject.h"

namespace {

using namespace std::chrono_literals;
constexpr auto InputTimeout = 250ms;
constexpr auto PublishInterval = 50ms;
constexpr float SoldierEyeHeight = 1.62f;
constexpr float Gravity = 9.8f;
// NetworkObjectMgrClass reserves 100,000 client object IDs per peer.
constexpr int ClientObjectBase = 2110000001;
constexpr int ClientObjectCount = 100000;
constexpr UINT ObjectHeaderBits = 32 + 8 + 1;
constexpr UINT HumanStateBits = 5;
constexpr UINT HumanSubStateBits = 9;
constexpr double PositionResolution = 0.2;
constexpr ULONG FirePrimary = 1u << (ControlClass::BOOLEAN_WEAPON_FIRE_PRIMARY - ControlClass::BOOLEAN_CONTINUOUS_FIRST);
constexpr ULONG FireSecondary = 1u << (ControlClass::BOOLEAN_WEAPON_FIRE_SECONDARY - ControlClass::BOOLEAN_CONTINUOUS_FIRST);
constexpr float InstantBulletSpeed = 400.0f; // BulletManager::Create_Bullet.
constexpr std::size_t MaxActiveBullets = 4096;
constexpr std::size_t MaxActiveCharges = 4096;

int Building_State(const HeadlessLevel::Building &building, float health)
{
    return BuildingStateClass::Compose_State(
        BuildingStateClass::Percentage_To_Health_State(100.0f * health / building.HealthMax), building.Powered);
}

Vector2 Left_Of(const Vector2 &forward)
{
    return Vector2(-forward.Y, forward.X);
}

} // namespace

HeadlessPlayerSession::HeadlessPlayerSession(const HeadlessCollision &collision, cConnection &connection)
    : Simulation(collision), Connection(connection)
{
    AnalogEncoder.Init(-1.0, 1.0, 0.01); // ControlClass::Set_Precision
    VitalsEncoder.Init(0, 2000, 1); // DefenseObjectClass::Set_Precision
    ArmorEncoder.Init(0, 1, 1);
    BuildingRadiusEncoder.Init(0, 50, 0.1);
    BuildingStateEncoder.Init(-1, BuildingStateClass::STATE_COUNT, 1);
    ChargeVelocityEncoder.Init(-90, 90, 0.01); // VehicleGameObj::Set_Precision.
    Init_Position_Encoders(collision, PositionEncoders);
}

void HeadlessPlayerSession::Set_Combat_Rules(const HeadlessArmorTable &armor, bool friendly_fire)
{
    Armor = &armor;
    FriendlyFire = friendly_fire;
    ArmorEncoder.Init(0, std::max<std::size_t>(1, armor.size()), 1);
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

bool HeadlessPlayerSession::Attach_Player(int peer_id, int player_type, int soldier_object_id,
    const HeadlessSoldierPreset &preset)
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
    peer.Health = preset.Health;
    peer.Shield = preset.Shield;
    peer.Skin = preset.Skin;
    peer.ShieldType = preset.ShieldType;
    peer.DamagePoints = preset.DamagePoints;
    peer.DeathPoints = preset.DeathPoints;
    peer.BullseyeHeight = preset.BullseyeHeight;
    peer.Weapons = preset.Weapons;
    const auto starting_weapon = std::find_if(peer.Weapons.begin(), peer.Weapons.end(), [&](const auto &weapon) {
        return weapon.DefinitionId == preset.StartingWeapon;
    });
    if (starting_weapon != peer.Weapons.end()) {
        peer.WeaponSlot = static_cast<int>(starting_weapon - peer.Weapons.begin()) + 1;
    }
    peer.Viewers.insert(peer_id);
    return true;
}

void HeadlessPlayerSession::Introduce_Soldier(int owner_id, int recipient, const HeadlessSoldierPreset &preset)
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

    Write_Occasional(packet, peer);
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
            peer.FireControls = 0;
            peer.ReloadRequested = false;
            Simulation.Set_Move_Input(peer_id, peer.MoveAxes);
        }
    }
}

bool HeadlessPlayerSession::Soldier_Is_Dead(int peer_id) const
{
    const auto peer = Peers.find(peer_id);
    return peer != Peers.end() && peer->second.SoldierObject != 0 && peer->second.Health == 0;
}

void HeadlessPlayerSession::Clear_Soldier(Peer &peer)
{
    // The client control object belongs to the connection and survives the body.
    peer = Peer{0, Vector2(0, 0), Vector3(0, 0, 0), peer.ControlObject};
    peer.Ready = false;
}

void HeadlessPlayerSession::Remove_Soldier(int peer_id)
{
    Simulation.Remove_Soldier(peer_id);
    if (const auto peer = Peers.find(peer_id); peer != Peers.end()) {
        Clear_Soldier(peer->second);
    }
}

void HeadlessPlayerSession::Remove_Player(int peer_id)
{
    Peers.erase(peer_id);
    Simulation.Remove_Soldier(peer_id);
    MapViewers.erase(peer_id);
    Charges.erase(std::remove_if(Charges.begin(), Charges.end(), [&](const auto &charge) {
        if (charge.OwnerPeer != peer_id && charge.AttachedPeer != peer_id) {
            return false;
        }
        Send_Map_Object_Deletion(charge.ObjectId);
        return true;
    }), Charges.end());
    for (auto &[id, peer] : Peers) {
        peer.Viewers.erase(peer_id);
    }
}

void HeadlessPlayerSession::Reset_Map(const HeadlessCollision &collision)
{
    End_Combat();
    Clear_Buildings();
    Init_Position_Encoders(collision, PositionEncoders);
    Simulation = HeadlessSimulation(collision);
    Hits.clear();
    PublishElapsed = {};
    for (auto &[id, peer] : Peers) {
        Clear_Soldier(peer);
    }
}

void HeadlessPlayerSession::Initialize_Buildings()
{
    const auto &definitions = Simulation.Map_Collision().Level().Buildings();
    Buildings.resize(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto &def = definitions[index];
        if (!Armor || Armor->empty() || def.HealthMax <= 0 || def.MeshPrefix.empty()) {
            continue;
        }
        Buildings[index] = {Allocate_Network_Object(), def.Health, Headless_Armor_Index(*Armor, def.SkinSaveId),
            Headless_Armor_Index(*Armor, def.ShieldSaveId), Building_State(def, def.Health), false};
    }
}

void HeadlessPlayerSession::Introduce_Map_Objects(int recipient)
{
    if (!MapViewers.insert(recipient).second) {
        return;
    }
    const auto &definitions = Simulation.Map_Collision().Level().Buildings();
    for (std::size_t index = 0; index < Buildings.size(); ++index) {
        if (Buildings[index].ObjectId == 0) {
            continue;
        }
        Send_Building(index, recipient, NetworkObjectClass::BIT_CREATION);
        // The client imports the create packet's animation state before the defense fields.
        if (Buildings[index].State != Building_State(definitions[index], definitions[index].HealthMax)) {
            Send_Building(index, recipient, NetworkObjectClass::BIT_RARE);
        }
    }
    for (const auto &charge : Charges) {
        Send_Charge(charge, recipient, NetworkObjectClass::BIT_CREATION);
    }
}

void HeadlessPlayerSession::Send_Map_Object_Deletion(int object)
{
    cPacket packet;
    packet.Add(object);
    packet.Add(static_cast<BYTE>(0));
    packet.Add(true);
    for (const int recipient : MapViewers) {
        if (Connection.Get_Remote_Host(recipient)) {
            Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
        }
    }
}

void HeadlessPlayerSession::End_Combat()
{
    for (const auto &charge : Charges) {
        Send_Map_Object_Deletion(charge.ObjectId);
    }
    Charges.clear();
    Bullets.clear();
    for (auto &[id, peer] : Peers) {
        if (peer.DefenseChanged) {
            Publish_Player(id, peer);
        }
    }
}

void HeadlessPlayerSession::Clear_Buildings()
{
    for (const auto &building : Buildings) {
        if (building.ObjectId != 0) {
            Send_Map_Object_Deletion(building.ObjectId);
        }
    }
    Buildings.clear();
    MapViewers.clear();
}

bool HeadlessPlayerSession::Base_Is_Destroyed(int team) const
{
    const auto &definitions = Simulation.Map_Collision().Level().Buildings();
    bool has_buildings = false;
    for (std::size_t index = 0; index < Buildings.size(); ++index) {
        if (definitions[index].PlayerType == team) {
            has_buildings = true;
            if (Buildings[index].ObjectId == 0 || Buildings[index].Health > 0) {
                return false;
            }
        }
    }
    return has_buildings;
}

void HeadlessPlayerSession::Receive_Control(cPacket &packet, int peer_id)
{
    const auto peer = Peers.find(peer_id);
    if (peer != Peers.end() && peer->second.Ready && peer->second.Health > 0) {
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
    ULONG one_time;
    packet.Get_Bits(one_time, ControlClass::NUM_BOOLEAN_ONE_TIME);
    ULONG bits;
    packet.Get_Bits(bits, ControlClass::NUM_BOOLEAN_CONTINUOUS);
    const auto fire_controls = bits & (FirePrimary | FireSecondary);
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
    const auto previous_slot = peer.WeaponSlot;
    Select_Weapon(peer, one_time);
    if (peer.WeaponSlot != previous_slot) {
        peer.WeaponAction = WeaponState::Switching;
        peer.WeaponTimeRemaining = WeaponSwitchSeconds;
        peer.ReloadRequested = false;
    }
    if (peer.WeaponSlot != 0 && (one_time & (1u << ControlClass::BOOLEAN_WEAPON_USE))) {
        auto &weapon = peer.Weapons[peer.WeaponSlot - 1];
        if (weapon.PrimaryFire && weapon.PrimaryFire->TimedCharge) {
            const auto modes = weapon.PrimaryFire->TimedCharge->FuseSeconds.size();
            weapon.DetonationMode = (weapon.DetonationMode + 1) % modes;
        }
    }
    peer.FireControls = fire_controls;
    peer.ReloadRequested |= (one_time & (1u << ControlClass::BOOLEAN_WEAPON_RELOAD)) != 0;
    return true;
}

void HeadlessPlayerSession::Advance_Weapon(int player_id, Peer &peer, float seconds, ULONG fire_controls, bool reload)
{
    if (peer.WeaponSlot == 0) {
        return;
    }
    auto &weapon = peer.Weapons[peer.WeaponSlot - 1];
    if (!weapon.PrimaryFire) {
        return;
    }
    const HeadlessFireMode *mode = nullptr;
    if (fire_controls & FirePrimary) {
        mode = &*weapon.PrimaryFire;
    } else if ((fire_controls & FireSecondary) && weapon.SecondaryFire) {
        mode = &*weapon.SecondaryFire;
    }
    const auto start_reload = [&] {
        peer.WeaponAction = WeaponState::Reloading;
        peer.WeaponTimeRemaining = weapon.ReloadSeconds;
    };
    if (reload && peer.WeaponAction == WeaponState::Ready && weapon.ReserveRounds != 0) {
        start_reload();
    }
    while (seconds > 0) {
        if (peer.WeaponAction == WeaponState::Ready) {
            if (weapon.ClipRounds == 0 && weapon.ReserveRounds != 0) {
                start_reload();
            } else if (mode && weapon.ClipRounds > 0) {
                peer.WeaponAction = WeaponState::Charging;
                peer.WeaponTimeRemaining = mode->ChargeSeconds;
            } else {
                return;
            }
        }
        if (peer.WeaponTimeRemaining > seconds) {
            peer.WeaponTimeRemaining -= seconds;
            return;
        }
        seconds -= peer.WeaponTimeRemaining;
        switch (peer.WeaponAction) {
        case WeaponState::Charging:
            peer.WeaponAction = WeaponState::Firing;
            peer.WeaponTimeRemaining = mode ? mode->ShotInterval : weapon.PrimaryFire->ShotInterval;
            if (mode) {
                weapon.ClipRounds = std::max(0, weapon.ClipRounds - mode->RoundCost);
                if (mode->Bullet) {
                    Fire_Bullet(player_id, peer, *mode->Bullet);
                } else if (mode->TimedCharge) {
                    Throw_Charge(player_id, peer, *mode->TimedCharge, weapon.DetonationMode);
                }
            }
            continue;
        case WeaponState::Reloading: {
            int added = weapon.ClipSize - weapon.ClipRounds;
            if (weapon.ReserveRounds >= 0) {
                added = std::min(added, weapon.ReserveRounds);
                weapon.ReserveRounds -= added;
            }
            weapon.ClipRounds += added;
            break;
        }
        default:
            break;
        }
        peer.WeaponAction = WeaponState::Ready;
        peer.WeaponTimeRemaining = 0;
    }
}

void HeadlessPlayerSession::Fire_Bullet(int player_id, const Peer &peer, const HeadlessBulletDefinition &definition)
{
    const auto &soldier = *Simulation.Find_Soldier(player_id);
    const auto start = soldier.Position + Vector3(0, 0, SoldierEyeHeight);
    Vector3 direction = peer.Target - start;
    if (direction.Length2() < 0.0001f) {
        return;
    }
    direction.Normalize();
    if (definition.Spread > 0) {
        auto random = std::uniform_real_distribution<float>(0, 1);
        const float theta = random(SpreadRandom) * 6.283185307f;
        const float angle = random(SpreadRandom) * definition.Spread;
        const Vector3 axis = std::abs(direction.Z) > 0.99f ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
        auto right = Vector3::Cross_Product(direction, axis);
        right.Normalize();
        const auto up = Vector3::Cross_Product(right, direction);
        direction = direction * std::cos(angle) + (right * std::cos(theta) + up * std::sin(theta)) * std::sin(angle);
    }
    Bullet bullet{{peer.SoldierObject, soldier.PlayerType, definition.Damage, definition.Warhead},
        start, direction, definition.Speed, definition.Range * 1.2f};
    if (definition.Speed >= InstantBulletSpeed) {
        Trace_Bullet(bullet, start + direction * definition.Range);
    } else if (Bullets.size() < MaxActiveBullets) {
        Bullets.push_back(bullet);
    }
}

bool HeadlessPlayerSession::Trace_Bullet(const Bullet &bullet, const Vector3 &end)
{
    const auto &collision = Simulation.Map_Collision();
    const auto terrain = collision.Cast_Ray(bullet.Position, end, COLLISION_TYPE_PROJECTILE);
    if (terrain && terrain->StartsInside) {
        return true;
    }
    const auto target = Cast_Soldiers(LineSegClass(bullet.Position, end), bullet.Attack.ShooterObject,
        terrain ? terrain->Fraction : 1.0f);
    if (target) {
        const auto &soldier = *Simulation.Find_Soldier(target->PeerId);
        if (FriendlyFire || soldier.PlayerType != bullet.Attack.Team) {
            Apply_Soldier_Damage(target->PeerId, Peers.at(target->PeerId), bullet.Attack);
        }
        return true;
    }
    if (terrain) {
        if (const auto owner = collision.Building_For_Object(terrain->ObjectIndex)) {
            Apply_Building_Damage(*owner, collision.Level().Static_Objects()[terrain->ObjectIndex].MCT, bullet.Attack);
        }
    }
    return terrain.has_value();
}

std::optional<HeadlessPlayerSession::SoldierIntersection> HeadlessPlayerSession::Cast_Soldiers(
    const LineSegClass &ray, int ignore_object, float limit) const
{
    CastResultStruct hit;
    hit.Fraction = limit;
    std::optional<SoldierIntersection> intersection;
    for (const auto &[id, peer] : Peers) {
        if (!peer.Ready || peer.Health <= 0 || peer.SoldierObject == ignore_object) {
            continue;
        }
        const auto &soldier = *Simulation.Find_Soldier(id);
        if (CollisionMath::Collide(ray, HeadlessSimulation::Soldier_Bounds(soldier.Position), &hit)) {
            intersection = SoldierIntersection{id, hit.Fraction};
            if (hit.StartBad) {
                break;
            }
        }
    }
    return intersection;
}

void HeadlessPlayerSession::Apply_Building_Damage(std::size_t index, bool mct, const Offense &attack)
{
    if (index >= Buildings.size()) {
        return;
    }
    const auto &def = Simulation.Map_Collision().Level().Buildings()[index];
    auto &building = Buildings[index];
    if (building.ObjectId == 0 || building.Health <= 0 || (!FriendlyFire && def.PlayerType == attack.Team)) {
        return;
    }
    const unsigned skin = mct ? def.MCTSkin : building.Skin;
    if (skin >= Armor->size() || attack.Warhead >= (*Armor)[skin].Warheads.size()) {
        return;
    }
    const float scale = std::max(0.0f, (*Armor)[skin].Warheads[attack.Warhead].Scale);
    // BuildingGameObj::Apply_Damage_Building bypasses shields and uses the component's skin.
    const float damage = std::min(building.Health, attack.Damage * scale);
    if (damage > 0) {
        building.Health -= damage;
        building.Changed = true;
        Hits.push_back({0, def.PlayerType, attack.ShooterObject, double(damage) * def.DamagePoints,
            def.DeathPoints, building.Health == 0});
    }
}

void HeadlessPlayerSession::Apply_Soldier_Damage(int player_id, Peer &peer, const Offense &attack)
{
    if (!Armor || peer.Skin >= Armor->size() || peer.ShieldType >= Armor->size()) {
        return;
    }
    const auto &skin = (*Armor)[peer.Skin].Warheads;
    const auto &shield = (*Armor)[peer.ShieldType].Warheads;
    const auto warhead = attack.Warhead;
    if (warhead >= skin.size() || warhead >= shield.size()) {
        return;
    }
    double damage = attack.Damage;
    double shield_damage = 0;
    const float previous_shield = peer.Shield;
    if (peer.Shield > 0) {
        const auto &rule = shield[warhead];
        const double absorbed = damage * std::clamp(rule.ShieldAbsorption, 0.0f, 1.0f);
        const double shield_loss = std::min<double>(peer.Shield, absorbed * std::max(0.0f, rule.Scale));
        peer.Shield -= static_cast<float>(shield_loss);
        shield_damage = rule.Scale > 0 ? shield_loss / rule.Scale : 0;
        // DefenseObjectClass sends the unabsorbed fraction and spillover through the skin multiplier.
        damage = std::max(0.0, damage - (rule.Scale > 0 ? shield_damage : absorbed));
    }
    const float previous_health = peer.Health;
    const float health_damage = static_cast<float>(std::min<double>(peer.Health, damage * std::max(0.0f, skin[warhead].Scale)));
    peer.Health -= health_damage;
    peer.DefenseChanged |= peer.Health != previous_health || peer.Shield != previous_shield;
    const bool fatal = peer.Health == 0;
    // Windows awards points for scaled health damage and shield damage in unscaled units.
    const double points = (health_damage + shield_damage) * peer.DamagePoints;
    if (points != 0 || fatal) {
        Hits.push_back({player_id, Simulation.Find_Soldier(player_id)->PlayerType,
            attack.ShooterObject, points, peer.DeathPoints, fatal});
    }
    if (fatal) {
        peer.MoveAxes.Set(0, 0);
        peer.FireControls = 0;
        peer.InputRemaining = {};
        peer.ReloadRequested = false;
        Simulation.Set_Move_Input(player_id, peer.MoveAxes);
    }
}

std::vector<HeadlessPlayerSession::CombatHit> HeadlessPlayerSession::Take_Combat_Hits()
{
    return std::exchange(Hits, {});
}

void HeadlessPlayerSession::Advance_Bullets(float seconds)
{
    Bullets.erase(std::remove_if(Bullets.begin(), Bullets.end(), [&](Bullet &bullet) {
        const float travel = std::min(bullet.RemainingTravel, seconds * bullet.Speed);
        const auto end = bullet.Position + bullet.Direction * travel;
        if (Trace_Bullet(bullet, end)) {
            return true;
        }
        bullet.Position = end;
        bullet.RemainingTravel -= travel;
        return bullet.RemainingTravel <= 0;
    }), Bullets.end());
}

void HeadlessPlayerSession::Throw_Charge(int owner, const Peer &peer,
    const HeadlessTimedChargeDefinition &definition, unsigned mode)
{
    if (Charges.size() >= MaxActiveCharges) {
        return;
    }
    const auto &soldier = *Simulation.Find_Soldier(owner);
    const auto origin = soldier.Position + Vector3(0, 0, SoldierEyeHeight);
    auto direction = peer.Target - origin;
    if (direction.Length2() < 0.0001f) {
        direction.Set(peer.Forward.X, peer.Forward.Y, 0);
    }
    direction.Normalize();
    const float facing = std::atan2(peer.Forward.Y, peer.Forward.X);
    const auto &charge = Charges.emplace_back(Charge{Allocate_Network_Object(), owner, peer.SoldierObject,
        soldier.PlayerType, definition, origin, definition.ThrowSpeed * direction, facing, definition.FuseSeconds[mode]});
    for (const int recipient : MapViewers) {
        if (Connection.Get_Remote_Host(recipient)) {
            Send_Charge(charge, recipient, NetworkObjectClass::BIT_CREATION);
        }
    }
}

void HeadlessPlayerSession::Move_Charge(Charge &charge, float seconds)
{
    const auto &collision = Simulation.Map_Collision();
    const float acceleration = -Gravity * charge.Definition.GravityScale;
    const auto end = charge.Position + seconds * charge.Velocity +
        Vector3(0, 0, 0.5f * acceleration * seconds * seconds);
    const auto surface = collision.Cast_Ray(charge.Position, end, COLLISION_TYPE_PROJECTILE, true);
    if (surface && surface->StartsInside) {
        charge.Position = end;
        charge.Velocity.Z += acceleration * seconds;
        return;
    }

    const LineSegClass ray(charge.Position, end);
    const auto target = Cast_Soldiers(ray, charge.OwnerObject, surface ? surface->Fraction : 1.0f);
    if (!surface && !target) {
        charge.Position = end;
        charge.Velocity.Z += acceleration * seconds;
        return;
    }

    // ProjectileClass stops just short of the contact plane.
    ray.Compute_Point(0.99f * (target ? target->Fraction : surface->Fraction), &charge.Position);
    charge.Velocity.Set(0, 0, 0);
    charge.Stuck = true;
    if (target) {
        const auto &peer = Peers.at(target->PeerId);
        charge.AttachedPeer = target->PeerId;
        charge.AttachedObject = peer.SoldierObject;
        const auto delta = charge.Position - Simulation.Find_Soldier(target->PeerId)->Position;
        const auto &forward = peer.Forward;
        charge.AttachedOffset.Set(Vector2::Dot_Product(Vector2(delta.X, delta.Y), forward),
            Vector2::Dot_Product(Vector2(delta.X, delta.Y), Left_Of(forward)), delta.Z);
    } else {
        const auto building = collision.Building_For_Object(surface->ObjectIndex);
        if (building && *building < Buildings.size()) {
            charge.Building = building;
            charge.StuckMCT = collision.Level().Static_Objects()[surface->ObjectIndex].MCT;
        }
    }
}

void HeadlessPlayerSession::Advance_Charges(double seconds, float movement_seconds)
{
    Charges.erase(std::remove_if(Charges.begin(), Charges.end(), [&](Charge &charge) {
        const auto owner = Peers.find(charge.OwnerPeer);
        const auto target = Peers.find(charge.AttachedPeer);
        if (owner == Peers.end() || (charge.AttachedPeer != 0 &&
                (target == Peers.end() || target->second.SoldierObject != charge.AttachedObject))) {
            Send_Map_Object_Deletion(charge.ObjectId);
            return true;
        }
        const bool was_stuck = charge.Stuck;
        const int previous_owner = charge.OwnerObject;
        charge.OwnerObject = owner->second.SoldierObject;
        if (!charge.Stuck) {
            Move_Charge(charge, static_cast<float>(std::min<double>(movement_seconds, charge.FuseRemaining)));
        } else if (charge.AttachedPeer != 0) {
            const auto &forward = target->second.Forward;
            const auto xy = charge.AttachedOffset.X * forward + charge.AttachedOffset.Y * Left_Of(forward);
            charge.Position = Simulation.Find_Soldier(charge.AttachedPeer)->Position +
                Vector3(xy.X, xy.Y, charge.AttachedOffset.Z);
        }
        charge.FuseRemaining -= seconds;
        if (charge.FuseRemaining <= 0) {
            Detonate_Charge(charge);
            Send_Map_Object_Deletion(charge.ObjectId);
            return true;
        }
        if (charge.Stuck != was_stuck || charge.OwnerObject != previous_owner) {
            for (const int recipient : MapViewers) {
                if (Connection.Get_Remote_Host(recipient)) {
                    Send_Charge(charge, recipient, NetworkObjectClass::BIT_RARE);
                }
            }
        }
        return false;
    }), Charges.end());
}

void HeadlessPlayerSession::Detonate_Charge(const Charge &charge)
{
    const auto &blast = charge.Definition.Explosion;
    const auto &collision = Simulation.Map_Collision();
    cPacket event;
    event.Add(Allocate_Network_Object());
    event.Add(static_cast<BYTE>(NetworkObjectClass::BIT_CREATION));
    event.Add(true);
    event.Add(NETCLASSID_SCEXPLOSIONEVENT);
    event.Add(blast.Id);
    Write_Position(event, charge.Position);
    event.Add(charge.OwnerObject);
    for (const int recipient : MapViewers) {
        if (Connection.Get_Remote_Host(recipient) && Peers.at(recipient).Ready) {
            Connection.Send_Packet_To_Individual(event, recipient, SEND_RELIABLE);
        }
    }

    for (auto &[id, peer] : Peers) {
        if (!peer.Ready || peer.Health <= 0) {
            continue;
        }
        const auto &soldier = *Simulation.Find_Soldier(id);
        if (soldier.PlayerType == charge.Team && id != charge.OwnerPeer && !FriendlyFire) {
            continue;
        }
        const auto bullseye = soldier.Position + Vector3(0, 0, peer.BullseyeHeight);
        const float distance = (bullseye - charge.Position).Length();
        if (blast.Radius <= 0 || (distance > blast.Radius && id != charge.AttachedPeer)) {
            continue;
        }
        float scale = 1.0f;
        if (id != charge.AttachedPeer) {
            if (blast.Scaled) {
                scale = 1.0f - distance / blast.Radius;
            }
            const auto cover = collision.Cast_Ray(bullseye, charge.Position, COLLISION_TYPE_PROJECTILE);
            if (cover && cover->Fraction < 1.0f - 0.00001f) {
                scale *= 0.25f;
            }
        }
        Apply_Soldier_Damage(id, peer, {charge.OwnerObject, charge.Team, blast.Damage * scale, blast.Warhead});
    }
    if (charge.Building) {
        Apply_Building_Damage(*charge.Building, charge.StuckMCT,
            {charge.OwnerObject, charge.Team, blast.Damage, blast.Warhead});
    }
}

void HeadlessPlayerSession::Send_Charge(const Charge &charge, int recipient, BYTE dirty)
{
    cPacket packet;
    packet.Add(charge.ObjectId);
    packet.Add(dirty);
    packet.Add(false);
    if (dirty == NetworkObjectClass::BIT_CREATION) {
        packet.Add(NETCLASSID_GAMEOBJ);
        packet.Add(charge.Definition.ObjectPreset);
        Write_Position(packet, charge.Position);
        packet.Add(charge.Facing);
    }
    packet.Add_Terminated_String(charge.Definition.Model.c_str(), true); // PhysicalGameObj::Export_Rare.
    packet.Add_Terminated_String("", true); // Animation.
    packet.Add(0); // Current frame.
    packet.Add(0); // Target frame.
    packet.Add(3); // ANIM_MODE_TARGET.
    packet.Add(0); // Host object.
    packet.Add(0); // Host bone.
    packet.Add(charge.Team);
    packet.Add(false); // HUD poke indicator.
    packet.Add(charge.Definition.AmmoPreset);
    packet.Add(charge.OwnerObject);
    const auto write_velocity = [&](const Vector3 &value) {
        for (int axis = 0; axis < 3; ++axis) {
            ULONG code;
            ChargeVelocityEncoder.Scale(value[axis], code);
            packet.Add_Bits(code, ChargeVelocityEncoder.Get_Bit_Precision());
        }
    };
    write_velocity(charge.Velocity);
    packet.Add(charge.Stuck);
    if (charge.Stuck) {
        Write_Position(packet, charge.Position);
        packet.Add(charge.StuckMCT);
        packet.Add(charge.AttachedObject != 0);
        packet.Add(charge.Building ? Buildings[*charge.Building].ObjectId : charge.AttachedObject);
        if (charge.AttachedObject != 0) {
            write_velocity(charge.AttachedOffset);
            packet.Add(0); // Use the soldier's root bone.
        }
        packet.Add(false); // No animated static object attachment.
    }
    Write_Defense(packet, charge.Definition.Health, charge.Definition.Shield, charge.Definition.ShieldType);
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessPlayerSession::Select_Weapon(Peer &peer, ULONG controls)
{
    const auto slots = static_cast<int>(peer.Weapons.size()) + 1;
    const auto select = [&](int direction, int key) {
        for (int step = 1; step < slots; ++step) {
            const int slot = (peer.WeaponSlot + direction * step + slots) % slots;
            if (slot != 0) {
                const float number = peer.Weapons[slot - 1].KeyNumber;
                if (key < 0 || (number > -1 && number < 10 && static_cast<int>(number) == key)) {
                    peer.WeaponSlot = slot;
                    return;
                }
            }
        }
    };
    const auto pressed = [controls](int button) { return (controls & (1u << button)) != 0; };
    if (pressed(ControlClass::BOOLEAN_WEAPON_NEXT)) {
        select(1, -1);
    }
    if (pressed(ControlClass::BOOLEAN_WEAPON_PREV)) {
        select(-1, -1);
    }
    if (pressed(ControlClass::BOOLEAN_SELECT_NO_WEAPON)) {
        peer.WeaponSlot = 0;
    }
    for (int key = 0; key <= 9; ++key) {
        if (pressed(ControlClass::BOOLEAN_SELECT_WEAPON_0 + key)) {
            select(1, key);
        }
    }
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
    const auto weapon_elapsed = std::min(elapsed, std::chrono::duration_cast<Duration>(100ms));
    const float movement_seconds = std::chrono::duration<float>(weapon_elapsed).count();
    Advance_Bullets(movement_seconds);
    Advance_Charges(std::chrono::duration<double>(elapsed).count(), movement_seconds);
    for (auto &[id, peer] : Peers) {
        if (peer.Ready && peer.Health > 0) {
            const auto held = std::min(weapon_elapsed, peer.InputRemaining);
            const bool reload = std::exchange(peer.ReloadRequested, false);
            Advance_Weapon(id, peer, std::chrono::duration<float>(held).count(), peer.FireControls, reload);
            Advance_Weapon(id, peer, std::chrono::duration<float>(weapon_elapsed - held).count(), 0, false);
        }
        if (peer.InputRemaining > Duration::zero()) {
            peer.InputRemaining -= std::min(elapsed, peer.InputRemaining);
            if (peer.InputRemaining == Duration::zero()) {
                peer.MoveAxes.Set(0, 0);
                peer.FireControls = 0;
                Simulation.Set_Move_Input(id, peer.MoveAxes);
            }
        }
    }
    Simulation.Advance(elapsed);
    Publish_Buildings();
    PublishElapsed += std::min(elapsed, std::chrono::duration_cast<Duration>(PublishInterval));
    if (PublishElapsed >= PublishInterval) {
        PublishElapsed -= PublishInterval;
        for (auto &[id, peer] : Peers) {
            Publish_Player(id, peer);
        }
    }
}

void HeadlessPlayerSession::Write_Defense(cPacket &packet, float health, float shield, unsigned shield_type)
{
    packet.Add(health == 0);
    ULONG code;
    VitalsEncoder.Scale(std::lround(health), code);
    packet.Add_Bits(code, VitalsEncoder.Get_Bit_Precision());
    VitalsEncoder.Scale(std::lround(shield), code);
    packet.Add_Bits(code, VitalsEncoder.Get_Bit_Precision());
    ArmorEncoder.Scale(shield_type, code);
    packet.Add_Bits(code, ArmorEncoder.Get_Bit_Precision());
}

void HeadlessPlayerSession::Write_Occasional(cPacket &packet, const Peer &peer)
{
    Write_Defense(packet, peer.Health, peer.Shield, peer.ShieldType);
    packet.Add(static_cast<int>(peer.Weapons.size()));
    for (const auto &weapon : peer.Weapons) {
        packet.Add(weapon.DefinitionId);
        packet.Add(weapon.Total_Rounds());
    }
}

void HeadlessPlayerSession::Send_Building(std::size_t index, int recipient, BYTE dirty)
{
    const auto &building = Buildings[index];
    const auto &def = Simulation.Map_Collision().Level().Buildings()[index];
    cPacket packet;
    packet.Add(building.ObjectId);
    packet.Add(dirty);
    packet.Add(false);
    ULONG code;
    if (dirty == NetworkObjectClass::BIT_CREATION) {
        packet.Add(NETCLASSID_GAMEOBJ);
        packet.Add(def.DefinitionId);
        Write_Position(packet, def.Position);
        Write_Position(packet, def.CollectionCenter);
        BuildingRadiusEncoder.Scale(def.CollectionRadius, code);
        packet.Add_Bits(code, BuildingRadiusEncoder.Get_Bit_Precision());
    }
    if ((dirty & NetworkObjectClass::BIT_RARE) == NetworkObjectClass::BIT_RARE) {
        packet.Add(building.Health == 0);
        packet.Add(def.Powered);
        BuildingStateEncoder.Scale(building.State, code);
        packet.Add_Bits(code, BuildingStateEncoder.Get_Bit_Precision());
    }
    Write_Defense(packet, building.Health, def.Shield, building.ShieldType);
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessPlayerSession::Publish_Buildings()
{
    const auto &definitions = Simulation.Map_Collision().Level().Buildings();
    for (std::size_t index = 0; index < Buildings.size(); ++index) {
        auto &building = Buildings[index];
        if (!building.Changed) {
            continue;
        }
        const int old = building.State;
        building.State = Building_State(definitions[index], building.Health);
        for (const int recipient : MapViewers) {
            if (Connection.Get_Remote_Host(recipient)) {
                Send_Building(index, recipient, NetworkObjectClass::BIT_OCCASIONAL);
                if (building.State != old) {
                    Send_Building(index, recipient, NetworkObjectClass::BIT_RARE);
                }
            }
        }
        building.Changed = false;
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
    packet.Add(peer.WeaponSlot != 0);
    ULONG fire_controls = 0;
    if (peer.WeaponSlot != 0) {
        const auto &weapon = peer.Weapons[peer.WeaponSlot - 1];
        packet.Add(weapon.DefinitionId);
        packet.Add(weapon.Total_Rounds());
        if (weapon.PrimaryFire) { fire_controls |= peer.FireControls & FirePrimary; }
        if (weapon.SecondaryFire) { fire_controls |= peer.FireControls & FireSecondary; }
    }
    Write_Position(packet, soldier.Position);
    const bool dead = peer.Health == 0;
    const auto state = dead ? HumanStateClass::DEATH :
        (soldier.Grounded ? HumanStateClass::UPRIGHT : HumanStateClass::AIRBORNE);
    packet.Add_Bits(state, HumanStateBits);
    const auto left = Left_Of(peer.Forward);
    const float forward_speed = soldier.Velocity.X * peer.Forward.X + soldier.Velocity.Y * peer.Forward.Y;
    const float left_speed = soldier.Velocity.X * left.X + soldier.Velocity.Y * left.Y;
    ULONG stance = 0;
    if (forward_speed > 0.01f) { stance |= HumanStateClass::SUB_STATE_FORWARD; }
    if (forward_speed < -0.01f) { stance |= HumanStateClass::SUB_STATE_BACKWARD; }
    if (left_speed > 0.01f) { stance |= HumanStateClass::SUB_STATE_LEFT; }
    if (left_speed < -0.01f) { stance |= HumanStateClass::SUB_STATE_RIGHT; }
    packet.Add_Bits(dead ? HumanStateClass::TORSO_FROM_FRONT : stance, HumanSubStateBits);
    if (state == HumanStateClass::AIRBORNE) {
        packet.Add(soldier.Velocity.X);
        packet.Add(soldier.Velocity.Y);
        packet.Add(soldier.Velocity.Z);
    }
    packet.Add(false); // Special damage.
    packet.Add(false); // Attached to a host bone.
    Write_Position(packet, peer.Target);
    packet.Add_Bits(fire_controls, ControlClass::NUM_BOOLEAN_CONTINUOUS);
    const float analog[] = {peer.MoveAxes.X, peer.MoveAxes.Y, 0, 0};
    for (const auto value : analog) {
        ULONG bits;
        AnalogEncoder.Scale(value, bits);
        packet.Add_Bits(bits, AnalogEncoder.Get_Bit_Precision());
    }
}

void HeadlessPlayerSession::Publish_Player(int peer_id, Peer &peer)
{
    if (!peer.Ready) {
        return;
    }
    cPacket packet;
    packet.Add(peer.SoldierObject);
    packet.Add(static_cast<BYTE>(peer.DefenseChanged ? NetworkObjectClass::BIT_OCCASIONAL : NetworkObjectClass::BIT_FREQUENT));
    packet.Add(false);
    if (peer.DefenseChanged) {
        Write_Occasional(packet, peer);
    }
    Write_Frequent(packet, peer_id, peer);
    for (const int recipient : peer.Viewers) {
        const auto viewer = Peers.find(recipient);
        if (viewer != Peers.end() && viewer->second.Ready && Connection.Get_Remote_Host(recipient)) {
            Connection.Send_Packet_To_Individual(packet, recipient, peer.DefenseChanged ? SEND_RELIABLE : SEND_UNRELIABLE);
        }
    }
    peer.DefenseChanged = false;
}
