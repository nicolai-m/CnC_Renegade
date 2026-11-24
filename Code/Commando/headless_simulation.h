// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_SIMULATION_H
#define HEADLESS_SIMULATION_H

#include <chrono>
#include <map>
#include <optional>

#include "vector2.h"
#include "vector3.h"

class HeadlessCollision;

class HeadlessSimulation {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds TickInterval{20};

    struct Soldier {
        int PlayerType;
        int SpawnerId;
        Vector3 Position; // Feet in world space.
        bool Grounded;
    };

    explicit HeadlessSimulation(const HeadlessCollision &collision);

    bool Spawn_Soldier(int player_id, int player_type);
    const Soldier *Find_Soldier(int player_id) const;

    // World-space movement axes in [-1, 1], held until the next input.
    bool Set_Move_Input(int player_id, const Vector2 &direction);
    void Advance(Clock::duration elapsed);

private:
    struct Actor {
        Soldier State;
        Vector2 MoveInput{0, 0};
        float VerticalSpeed = 0;
    };

    std::optional<Vector3> Find_Stand_Position(const Vector3 &position, float drop) const;
    void Slide_Soldier(Soldier &soldier, Vector3 move) const;
    void Tick();

    const HeadlessCollision &Collision;
    std::map<int, Actor> Soldiers;
    Clock::duration TimeAccumulated = Clock::duration::zero();
};

#endif // HEADLESS_SIMULATION_H
