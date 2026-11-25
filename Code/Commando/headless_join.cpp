// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_join.h"

#include <algorithm>
#include <utility>

#include "crc.h"
#include "netclassids.h"
#include "networkobject.h"
#include "playertype.h"

namespace {

constexpr unsigned MaxNickname = 64;
constexpr BYTE LanMessagePosition = 0; // LAN_MESSAGE_POSITION.
constexpr int CncGameType = 2; // cGameData::GAME_TYPE_CNC.
constexpr int TimeExpired = 2; // cGameData::WIN_TYPE_TIME.
constexpr int UnknownPlayer = -99999; // PLAYER_ID_UNKNOWN.
constexpr std::chrono::seconds IntermissionDuration{10};

template<class Char>
bool Read_String(cPacket &packet, std::basic_string<Char> &output, unsigned limit)
{
    USHORT length;
    constexpr unsigned char_bits = sizeof(Char) * 8;
    if (packet.Get_Bits_Remaining() < 16 || packet.Get(length) > limit ||
        packet.Get_Bits_Remaining() < length * char_bits) {
        return false;
    }
    output.clear();
    for (unsigned i = 0; i < length; ++i) {
        ULONG unit;
        packet.Get_Bits(unit, char_bits);
        if (unit == 0) {
            return false;
        }
        output.push_back(static_cast<Char>(unit));
    }
    return true;
}

template<class Char>
bool Same_Ascii_Name(const std::basic_string<Char> &left, const std::basic_string<Char> &right)
{
    auto fold = [](Char c) { return c >= 'a' && c <= 'z' ? static_cast<Char>(c - 'a' + 'A') : c; };
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [&](Char a, Char b) { return fold(a) == fold(b); });
}

void Write_Header(cPacket &packet, int object, BYTE dirty, bool deleting)
{
    packet.Add(object);
    packet.Add(dirty);
    packet.Add(deleting);
}

void Write_Creation(cPacket &packet, int object, int class_id, bool transient = false)
{
    Write_Header(packet, object, NetworkObjectClass::BIT_CREATION, transient);
    packet.Add(class_id);
}

uint32 Map_CRC(std::string map)
{
    for (auto &c : map) {
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
    }
    return CRC::String(map.c_str());
}

} // namespace

HeadlessJoin::HeadlessJoin(cConnection &connection, HeadlessPlayerSession &session,
    const std::vector<HeadlessMap> &maps, const HeadlessJoinOptions &options)
    : Connection(connection), Session(session), Maps(maps), Options(options), MapCrc(Map_CRC(maps.at(0).Name)),
      FrameTime(Clock::now()), RoundEnd(FrameTime + std::chrono::minutes(options.TimeLimit)),
      Teams{NextObject--, NextObject--}
{
}

bool HeadlessJoin::Update_Round(Clock::time_point now)
{
    FrameTime = now;
    if (IntermissionEnd) {
        if (now >= *IntermissionEnd) {
            if (CycleEnding) {
                return false;
            }
            Restart_Round();
        }
    } else if (Options.TimeLimit > 0 && now >= RoundEnd) {
        Begin_Intermission();
    }
    return true;
}

void HeadlessJoin::Begin_Intermission()
{
    IntermissionEnd = FrameTime + IntermissionDuration;
    if (MapIndex + 1 < Maps.size()) {
        ++MapIndex;
    } else if (Options.LoopMaps) {
        MapIndex = 0;
    } else {
        CycleEnding = true;
    }
    MapCrc = Map_CRC(Maps[MapIndex].Name);
    for (auto &[id, peer] : Peers) {
        if (peer.PlayerObject != 0 && peer.State == LoadState::Ready) {
            Request_Reload(id, peer);
        }
    }
}

void HeadlessJoin::Restart_Round()
{
    IntermissionEnd.reset();
    RoundEnd = FrameTime + std::chrono::minutes(Options.TimeLimit);
    ++HostedGameNumber;
    for (auto &[id, peer] : Peers) {
        if (peer.SoldierObject != 0) {
            for (const auto &[recipient, other] : Peers) {
                if (other.PlayerObject != 0) {
                    Delete_Object(recipient, peer.SoldierObject);
                }
            }
            peer.SoldierObject = 0;
        }
    }
    Session.Reset_Map(Maps[MapIndex].Collision);
    for (auto &[id, peer] : Peers) {
        if (peer.State == LoadState::Ready) {
            Spawn_Player(id, peer);
        }
    }
}

REFUSAL_CODE HeadlessJoin::Accept(cPacket &packet)
{
    if (CycleEnding) {
        return REFUSAL_BY_APPLICATION;
    }
    std::u16string name;
    std::u16string password;
    UINT key;
    if (!Read_String(packet, name, MaxNickname) || name.empty() || !Read_String(packet, password, 255) ||
        packet.Get_Bits_Remaining() != 64) {
        return REFUSAL_VERSION_MISMATCH;
    }
    packet.Get(key);
    if (!Options.Password.empty() && password != Options.Password) {
        return REFUSAL_BAD_PASSWORD;
    }
    if (key != Maps[MapIndex].Join.VersionKey) {
        return REFUSAL_VERSION_MISMATCH;
    }
    // cConnection reads the bandwidth after this callback returns.
    cPacket remaining;
    remaining = packet;
    int bandwidth;
    if (remaining.Get(bandwidth) <= 0) {
        return REFUSAL_BY_APPLICATION;
    }
    if (std::any_of(Peers.begin(), Peers.end(), [&](const auto &other) {
        return Same_Ascii_Name(name, other.second.Name);
    })) {
        return REFUSAL_PLAYER_EXISTS;
    }
    PendingName = std::move(name);
    return REFUSAL_CLIENT_ACCEPTED;
}

void HeadlessJoin::Connected(int peer_id)
{
    auto &peer = Peers.try_emplace(peer_id).first->second;
    peer.Name = std::move(PendingName);
    peer.MapIndex = MapIndex;
    Connection.Set_Rhost_Is_In_Game(peer_id, false);
    Send_Team(peer_id, PLAYERTYPE_NOD);
    Send_Team(peer_id, PLAYERTYPE_GDI);
    Send_Game_Options(peer_id);
}

void HeadlessJoin::Receive(cPacket &packet, int peer_id)
{
    const auto found = Peers.find(peer_id);
    if (found == Peers.end() || Closing.count(peer_id) != 0 || packet.Get_Bits_Remaining() < 41) {
        packet.Flush();
        return;
    }
    auto &peer = found->second;

    cPacket payload;
    payload = packet;
    int object;
    BYTE dirty;
    bool deleting;
    payload.Get(object);
    payload.Get(dirty);
    payload.Get(deleting);
    if (!HeadlessPlayerSession::Client_Owns_Object_Id(peer_id, object)) {
        packet.Flush();
        return;
    }

    if (dirty != NetworkObjectClass::BIT_CREATION) {
        Session.Receive_Control(packet, peer_id);
        return;
    }
    if (packet.Get_Type() != PACKETTYPE_RELIABLE || payload.Get_Bits_Remaining() < 64) {
        packet.Flush();
        return;
    }
    int class_id;
    int owner;
    payload.Get(class_id);
    if (payload.Get(owner) != peer_id) {
        packet.Flush();
        return;
    }
    if (class_id == NETCLASSID_CLIENTCONTROL) {
        if (peer.SoldierObject != 0) {
            Session.Receive_Control(packet, peer_id);
            return;
        }
        int soldier;
        if (!deleting && payload.Get_Bits_Remaining() == 32 && payload.Get(soldier) == -1) {
            peer.InitialControl = std::make_unique<cPacket>();
            *peer.InitialControl = packet;
        }
    } else if (class_id == NETCLASSID_BIOEVENT) {
        Receive_Bio(payload, peer_id, peer);
    } else if (class_id == NETCLASSID_LOADINGEVENT && payload.Get_Bits_Remaining() == 1) {
        bool loading;
        Set_Loading(peer_id, peer, payload.Get(loading));
    } else if (class_id == NETCLASSID_CLIENTGOODBYEEVENT && payload.Is_Flushed()) {
        Closing.insert(peer_id);
    }
    packet.Flush();
}

void HeadlessJoin::Receive_Bio(cPacket &packet, int peer_id, Peer &peer)
{
    if (peer.Team >= 0) {
        return;
    }
    std::u16string nickname;
    std::string map;
    if (!Read_String(packet, nickname, MaxNickname) || !Same_Ascii_Name(nickname, peer.Name) ||
        packet.Get_Bits_Remaining() < 64) {
        return;
    }
    int team;
    UINT clan;
    packet.Get(team);
    packet.Get(clan);
    if (!Read_String(packet, map, 255) || !packet.Is_Flushed() || !Same_Ascii_Name(map, Maps[peer.MapIndex].Name)) {
        return;
    }
    if (team != PLAYERTYPE_NOD && team != PLAYERTYPE_GDI) {
        const auto balance = std::count_if(Peers.begin(), Peers.end(), [](const auto &entry) {
            return entry.second.Team == PLAYERTYPE_NOD;
        }) - std::count_if(Peers.begin(), Peers.end(), [](const auto &entry) {
            return entry.second.Team == PLAYERTYPE_GDI;
        });
        team = balance > 0 ? PLAYERTYPE_GDI : PLAYERTYPE_NOD;
    }
    peer.Team = team;
    if (!Same_Ascii_Name(map, Maps[MapIndex].Name)) {
        Request_Reload(peer_id, peer);
    } else {
        peer.State = LoadState::Ready;
        if (!IntermissionEnd) {
            Spawn_Player(peer_id, peer);
        }
    }
}

void HeadlessJoin::Create_Player(int peer_id, Peer &peer)
{
    peer.PlayerObject = NextObject--;
    for (const auto &[id, other] : Peers) {
        if (other.PlayerObject != 0) {
            Send_Player(peer_id, id, other, true);
            if (id != peer_id) {
                Send_Player(id, peer_id, peer, true);
            }
        }
    }
}

void HeadlessJoin::Request_Reload(int peer_id, Peer &peer)
{
    peer.MapIndex = MapIndex;
    peer.State = LoadState::AwaitingReload;
    Connection.Set_Rhost_Is_In_Game(peer_id, false);
    Session.Set_Player_Loading(peer_id, true);
    if (peer.PlayerObject == 0) {
        Create_Player(peer_id, peer);
    }
    Send_Round_Result(peer_id);
}

void HeadlessJoin::Spawn_Player(int peer_id, Peer &peer)
{
    if (!Session.Attach_Player(peer_id, peer.Team, NextObject, Maps[MapIndex].Join.Soldiers[peer.Team])) {
        Closing.insert(peer_id);
        return;
    }
    peer.SoldierObject = NextObject--;
    const bool joining = peer.PlayerObject == 0;
    if (joining) {
        Create_Player(peer_id, peer);
    }
    if (peer.InitialControl) {
        Session.Receive_Control(*peer.InitialControl, peer_id);
        peer.InitialControl.reset();
    }
    Connection.Set_Rhost_Is_In_Game(peer_id, true);
    for (const auto &[id, other] : Peers) {
        if (other.PlayerObject == 0) {
            continue;
        }
        if (!joining && other.State == LoadState::Ready) {
            Send_Player(id, peer_id, peer, false);
        }
        if (other.SoldierObject != 0) {
            Introduce_Soldier(peer_id, id, other);
            if (id != peer_id) {
                Introduce_Soldier(id, peer_id, peer);
            }
        }
    }
    Send_Game_Data(peer_id);
}

void HeadlessJoin::Set_Loading(int peer_id, Peer &peer, bool loading)
{
    if (peer.PlayerObject == 0 || (loading ? peer.State == LoadState::Loading : peer.State != LoadState::Loading)) {
        return; // Initial readiness comes from cBioEvent after Load_Level.
    }
    if (!loading && !CycleEnding && !Same_Ascii_Name(Maps[peer.MapIndex].Name, Maps[MapIndex].Name)) {
        Request_Reload(peer_id, peer);
        return;
    }
    peer.State = loading ? LoadState::Loading : LoadState::Ready;
    if (IntermissionEnd) {
        return;
    }
    if (!loading && peer.SoldierObject == 0) {
        Spawn_Player(peer_id, peer);
        return;
    }
    Session.Set_Player_Loading(peer_id, loading);
    Connection.Set_Rhost_Is_In_Game(peer_id, !loading);
    for (const auto &[id, other] : Peers) {
        if (other.PlayerObject != 0 && other.State == LoadState::Ready) {
            Send_Player(id, peer_id, peer, false);
        }
    }
    if (!loading) {
        Send_Game_Data(peer_id);
    }
}

void HeadlessJoin::Disconnected(int peer_id)
{
    const auto found = Peers.find(peer_id);
    if (found == Peers.end()) {
        return;
    }
    const auto player = found->second.PlayerObject;
    const auto soldier = found->second.SoldierObject;
    Peers.erase(found);
    Session.Remove_Player(peer_id);
    if (player != 0) {
        for (const auto &[id, other] : Peers) {
            if (other.PlayerObject == 0) {
                continue;
            }
            Delete_Object(id, soldier);
            Delete_Object(id, player);
        }
    }
}

void HeadlessJoin::Close_Pending()
{
    for (const auto id : Closing) {
        Connection.Destroy_Connection(id);
        Disconnected(id);
    }
    Closing.clear();
}

void HeadlessJoin::Send_Team(int recipient, int team)
{
    cPacket packet;
    Write_Creation(packet, Teams[team], NETCLASSID_TEAM);
    packet.Add(team);
    packet.Add(0); // Kills.
    packet.Add(0); // Deaths.
    packet.Add(0.0f); // Score.
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessJoin::Write_Lan_Announcement(cPacket &packet, uint32 address, uint32 sequence, bool hosting) const
{
    hosting = hosting && !CycleEnding;
    packet.Add(LanMessagePosition);
    packet.Add_Wide_Terminated_String(Options.Owner.c_str());
    packet.Add(sequence);
    packet.Add(hosting);
    if (hosting) {
        packet.Add(CncGameType);
        Write_Game_Info(packet, address);
    }
}

void HeadlessJoin::Write_Game_Info(cPacket &packet, uint32 address) const
{
    const auto &data = Maps[MapIndex].Join;
    packet.Add(address);
    packet.Add_Wide_Terminated_String(Options.Owner.c_str());
    packet.Add_Wide_Terminated_String(Options.Title.c_str(), true);
    packet.Add(static_cast<int>(Connection.Get_Local_Port()));
    packet.Add(static_cast<int>(Peers.size()));
    packet.Add(Connection.Get_Max_RHost());
    packet.Add(data.VersionKey);
    packet.Add(data.ExeCRC);
    packet.Add(data.StringsCRC);
    packet.Add(true); // Dedicated.
    packet.Add(false); // Team changing.
    packet.Add(!Options.Password.empty());
    packet.Add(false); // Ranked.
    packet.Add(false); // Clan game.
    packet.Add(MapCrc);
    packet.Add(0); // Mod CRC.
}

HeadlessJoin::Clock::duration HeadlessJoin::Time_Remaining() const
{
    return std::max(Clock::duration::zero(), RoundEnd - FrameTime);
}

void HeadlessJoin::Send_Game_Options(int recipient)
{
    cPacket packet;
    Write_Creation(packet, NextObject--, NETCLASSID_GAMEOPTIONSEVENT, true);
    Write_Game_Info(packet, Options.Address);
    packet.Add(Options.TimeLimit);
    packet.Add(Options.RadarMode);
    packet.Add(static_cast<int>(IntermissionDuration.count()));
    packet.Add(0); // Minimum qualifying minutes.
    for (int i = 0; i < 7; ++i) { // Tier 2 flags, including three C&C-specific flags.
        packet.Add(false);
    }
    packet.Add_Wide_Terminated_String(Options.Motd.c_str(), true);
    // Extra fields from cGameDataCnc::Export_Tier_2_Data.
    packet.Add(false); // Base destruction ends the game.
    packet.Add(false); // Beacon placement ends the game.
    packet.Add(0); // Starting credits.
    packet.Add(std::chrono::duration<float>(Time_Remaining()).count());
    packet.Add(HostedGameNumber);
    packet.Add(0); // Mod CRC.
    packet.Add(MapCrc);
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessJoin::Send_Game_Data(int recipient)
{
    cPacket packet;
    Write_Creation(packet, NextObject--, NETCLASSID_GAMEDATAUPDATEEVENT, true);
    packet.Add(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(Time_Remaining()).count()));
    packet.Add(HostedGameNumber);
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessJoin::Send_Round_Result(int recipient)
{
    cPacket packet;
    Write_Creation(packet, NextObject--, NETCLASSID_WIN, true);
    packet.Add(static_cast<int>(PLAYERTYPE_NOD)); // Both teams currently score zero. Break the tie by team order.
    packet.Add(UnknownPlayer);
    // Clients finishing a previous map after the next round began still need its current round number.
    packet.Add(IntermissionEnd ? HostedGameNumber : HostedGameNumber - 1);
    packet.Add(CycleEnding);
    packet.Add(TimeExpired);
    packet.Add(static_cast<uint32>(Options.TimeLimit * 60));
    packet.Add_Wide_Terminated_String(u"", true); // MVP name.
    packet.Add(0); // MVP count.
    packet.Add(0); // Mod CRC.
    packet.Add(MapCrc);
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessJoin::Send_Player(int recipient, int player_id, const Peer &peer, bool creation)
{
    cPacket packet;
    if (creation) {
        Write_Creation(packet, peer.PlayerObject, NETCLASSID_PLAYER);
        packet.Add_Wide_Terminated_String(peer.Name.c_str());
    } else {
        Write_Header(packet, peer.PlayerObject, NetworkObjectClass::BIT_RARE, false);
    }
    packet.Add(player_id);
    packet.Add(0); // Ladder points.
    packet.Add(peer.Team);
    packet.Add(100); // Damage scale.
    packet.Add(0); // Ping.
    packet.Add(peer.State == LoadState::Ready);
    packet.Add(false); // Invulnerable.
    packet.Add(true); // Active.
    packet.Add(0); // Rank.
    packet.Add(0); // Ranked games.
    packet.Add(0.0f); // Score.
    packet.Add(0.0f); // Money.
    packet.Add(0); // Kills.
    packet.Add(0); // Deaths.
    Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
}

void HeadlessJoin::Delete_Object(int recipient, int object)
{
    if (object != 0 && Connection.Get_Remote_Host(recipient)) {
        cPacket packet;
        Write_Header(packet, object, 0, true);
        Connection.Send_Packet_To_Individual(packet, recipient, SEND_RELIABLE);
    }
}

void HeadlessJoin::Introduce_Soldier(int recipient, int player_id, const Peer &peer)
{
    const auto &data = Maps[MapIndex].Join;
    Session.Introduce_Soldier(player_id, recipient, data.Soldiers[peer.Team], data.ArmorTypes);
}
