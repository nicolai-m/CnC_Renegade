// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_PLAYER_SESSION_H
#define HEADLESS_PLAYER_SESSION_H

#include <array>
#include <map>
#include <set>

#include "encodertypeentry.h"
#include "headless_simulation.h"

class cConnection;
class cPacket;
struct HeadlessSoldierPreset;

class HeadlessPlayerSession {
public:
    HeadlessPlayerSession(const HeadlessCollision &collision, cConnection &connection);

    // The joining client must know the soldier's network object before the first update.
    bool Attach_Player(int peer_id, int player_type, int soldier_object_id);
    void Introduce_Soldier(int owner_id, int recipient, const HeadlessSoldierPreset &preset, unsigned armor_types);
    void Set_Player_Loading(int peer_id, bool loading);
    void Remove_Player(int peer_id);
    void Reset_Soldiers();
    void Receive_Control(cPacket &packet, int peer_id);
    void Advance(HeadlessSimulation::Clock::duration elapsed);
    static bool Client_Owns_Object_Id(int peer_id, int object_id);

private:
    struct Peer {
        int SoldierObject;
        Vector2 Forward;
        Vector3 Target;
        int ControlObject = 0;
        Vector2 MoveAxes{0, 0};
        HeadlessSimulation::Clock::duration InputRemaining{};
        std::set<int> Viewers;
        bool Ready = true;
    };

    bool Read_Control(cPacket &packet, int peer_id, Peer &peer);
    void Write_Position(cPacket &packet, const Vector3 &position);
    void Write_Frequent(cPacket &packet, int peer_id, const Peer &peer);
    void Publish_Player(int peer_id, const Peer &peer);

    HeadlessSimulation Simulation;
    cConnection &Connection;
    cEncoderTypeEntry AnalogEncoder;
    std::array<cEncoderTypeEntry, 3> PositionEncoders;
    std::map<int, Peer> Peers;
    HeadlessSimulation::Clock::duration PublishElapsed{};
};

#endif // HEADLESS_PLAYER_SESSION_H
