// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_JOIN_H
#define HEADLESS_JOIN_H

#include <map>
#include <memory>
#include <set>
#include <string>

#include "connect.h"
#include "headless_join_data.h"
#include "headless_player_session.h"

struct HeadlessJoinOptions {
    std::string Map;
    std::u16string Owner = u"FDS";
    std::u16string Title = u"Dedicated Renegade Server";
    std::u16string Motd;
    std::u16string Password;
    uint32 Address = 0;
    int TimeLimit = 30;
    int RadarMode = 1;
};

class HeadlessJoin {
public:
    HeadlessJoin(cConnection &connection, HeadlessPlayerSession &session, const HeadlessJoinData &data,
        const HeadlessJoinOptions &options);

    REFUSAL_CODE Accept(cPacket &packet);
    void Connected(int peer_id);
    void Receive(cPacket &packet, int peer_id);
    void Disconnected(int peer_id);
    void Close_Pending();
    void Write_Lan_Announcement(cPacket &packet, uint32 address, uint32 sequence, bool hosting) const;

private:
    struct Peer {
        std::u16string Name;
        std::unique_ptr<cPacket> InitialControl;
        int Team = -1;
        int PlayerObject = 0;
        int SoldierObject = 0;
        bool Loading = true;
    };

    void Receive_Bio(cPacket &packet, int peer_id, Peer &peer);
    void Set_Loading(int peer_id, Peer &peer, bool loading);
    void Write_Game_Info(cPacket &packet, uint32 address) const;
    void Send_Game_Options(int recipient);
    void Send_Game_Data(int recipient);
    void Send_Team(int recipient, int team);
    void Send_Player(int recipient, int player_id, const Peer &peer, bool creation);
    void Introduce_Soldier(int recipient, int player_id, const Peer &peer);

    cConnection &Connection;
    HeadlessPlayerSession &Session;
    const HeadlessJoinData &Data;
    const HeadlessJoinOptions &Options;
    const uint32 MapCrc;
    std::u16string PendingName;
    std::map<int, Peer> Peers;
    std::set<int> Closing;
    // Allocate from the high end of the original dynamic object range.
    int NextObject = 2100000000;
    const std::array<int, 2> Teams;
};

#endif // HEADLESS_JOIN_H
