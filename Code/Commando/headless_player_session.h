// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_PLAYER_SESSION_H
#define HEADLESS_PLAYER_SESSION_H

#include <array>
#include <map>

#include "encodertypeentry.h"
#include "headless_simulation.h"

class cConnection;
class cPacket;

class HeadlessPlayerSession {
public:
    HeadlessPlayerSession(const HeadlessCollision &collision, cConnection &connection);

    // The joining client must know the soldier's network object before the first update.
    bool Attach_Player(int peer_id, int player_type, int soldier_object_id);
    void Remove_Player(int peer_id);
    void Receive_Control(cPacket &packet, int peer_id);
    void Advance(HeadlessSimulation::Clock::duration elapsed);

private:
    struct Peer {
        int SoldierObject;
        Vector2 Forward;
        Vector3 Target;
        int ControlObject = 0;
        Vector2 MoveAxes{0, 0};
        HeadlessSimulation::Clock::duration InputRemaining{};
    };

    bool Read_Control(cPacket &packet, int peer_id, Peer &peer);
    void Write_Position(cPacket &packet, const Vector3 &position);
    void Publish_Player(int peer_id, const Peer &peer);

    HeadlessSimulation Simulation;
    cConnection &Connection;
    cEncoderTypeEntry AnalogEncoder;
    std::array<cEncoderTypeEntry, 3> PositionEncoders;
    std::map<int, Peer> Peers;
    HeadlessSimulation::Clock::duration PublishElapsed{};
};

#endif // HEADLESS_PLAYER_SESSION_H
