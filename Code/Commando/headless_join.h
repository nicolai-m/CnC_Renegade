// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_JOIN_H
#define HEADLESS_JOIN_H

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "connect.h"
#include "headless_join_data.h"
#include "headless_player_session.h"

struct HeadlessMap {
    std::string Name;
    HeadlessCollision Collision;
    HeadlessJoinData Join;
};

struct HeadlessJoinOptions {
    std::u16string Owner = u"FDS";
    std::u16string Title = u"Dedicated Renegade Server";
    std::u16string Motd;
    std::u16string Password;
    uint32 Address = 0;
    int TimeLimit = 30;
    int RadarMode = 1;
    bool LoopMaps = true;
    bool FriendlyFire = false;
    bool BaseDestructionEndsGame = true;
};

class HeadlessJoin {
public:
    using Clock = std::chrono::steady_clock;

    HeadlessJoin(cConnection &connection, HeadlessPlayerSession &session, const std::vector<HeadlessMap> &maps,
        const HeadlessJoinOptions &options);

    bool Update_Round(Clock::time_point now);
    void Update_Combat();
    REFUSAL_CODE Accept(cPacket &packet);
    void Connected(int peer_id);
    void Receive(cPacket &packet, int peer_id);
    void Disconnected(int peer_id);
    void Close_Pending();
    void Write_Lan_Announcement(cPacket &packet, uint32 address, uint32 sequence, bool hosting) const;

private:
    enum class LoadState { Loading, Ready, AwaitingReload };

    struct CombatStats {
        int Kills = 0;
        int Deaths = 0;
        float Score = 0;
    };

    struct CombatChanges {
        std::set<int> Players;
        std::array<BYTE, 2> Teams{};
    };

    struct Peer {
        std::u16string Name;
        std::unique_ptr<cPacket> InitialControl;
        int Team = -1;
        int PlayerObject = 0;
        int SoldierObject = 0;
        std::size_t MapIndex = 0; // Map last sent to this player.
        LoadState State = LoadState::Loading;
        std::optional<Clock::time_point> RespawnAt;
        CombatStats Stats;
        float Credits = 0;
    };

    void Receive_Bio(cPacket &packet, int peer_id, Peer &peer);
    void Set_Loading(int peer_id, Peer &peer, bool loading);
    void Create_Player(int peer_id, Peer &peer);
    void Spawn_Player(int peer_id, Peer &peer, bool respawning = false);
    void Delete_Soldier(Peer &peer);
    void Request_Reload(int peer_id, Peer &peer);
    void Begin_Intermission(int winning_team, int reason);
    void Restart_Round();
    void Record_Hit(const HeadlessPlayerSession::CombatHit &hit, CombatChanges &changes);
    void Send_Combat_Stats(const CombatChanges &changes);
    void Reset_Combat_Stats();
    Clock::duration Time_Remaining() const;
    void Write_Game_Info(cPacket &packet, uint32 address) const;
    void Send_Game_Options(int recipient);
    void Send_Game_Data(int recipient);
    void Send_Round_Result(int recipient);
    void Send_Team(int recipient, int team, BYTE dirty);
    void Send_Player(int recipient, int player_id, const Peer &peer, BYTE dirty);
    void Introduce_Soldier(int recipient, int player_id, const Peer &peer);
    void Delete_Object(int recipient, int object);

    cConnection &Connection;
    HeadlessPlayerSession &Session;
    const std::vector<HeadlessMap> &Maps;
    const HeadlessJoinOptions &Options;
    std::size_t MapIndex = 0;
    uint32 MapCrc;
    bool CycleEnding = false;
    Clock::time_point FrameTime;
    Clock::time_point RoundStart;
    Clock::time_point RoundEnd;
    std::optional<Clock::time_point> IntermissionEnd;
    int HostedGameNumber = 1;
    int LastWinningTeam = 0;
    int LastWinReason = 2;
    uint32 LastRoundSeconds = 0;
    std::u16string PendingName;
    std::map<int, Peer> Peers;
    std::set<int> Closing;
    const std::array<int, 2> Teams;
    std::array<CombatStats, 2> TeamStats;
};

#endif // HEADLESS_JOIN_H
