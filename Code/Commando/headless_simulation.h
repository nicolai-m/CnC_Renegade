// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_SIMULATION_H
#define HEADLESS_SIMULATION_H

#include <chrono>
#include <functional>
#include <map>
#include <optional>

#include "vector2.h"
#include "vector3.h"

class HeadlessCollision;
class AABoxClass;

class HeadlessSimulation {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds TickInterval{20};

    struct Soldier {
        int PlayerType;
        int SpawnerId;
        Vector3 Position; // Feet in world space.
        Vector3 Velocity;
        float SpawnHeading;
        bool Grounded;
    };

    explicit HeadlessSimulation(const HeadlessCollision &collision);

    bool Spawn_Soldier(int player_id, int player_type);
    void Remove_Soldier(int player_id);
    const Soldier *Find_Soldier(int player_id) const;
    static AABoxClass Soldier_Bounds(const Vector3 &feet);
    const HeadlessCollision &Map_Collision() const { return Collision.get(); }

    // World-space movement axes in [-1, 1], held until the next input.
    bool Set_Move_Input(int player_id, const Vector2 &direction);
    void Set_Soldier_Vehicle_State(int player_id, const Vector3 &position, bool seated);
    std::optional<Vector3> Find_Stand_Position(const Vector3 &position, float drop) const;
    void Advance(Clock::duration elapsed);

private:
    struct Actor {
        Soldier State;
        Vector2 MoveInput{0, 0};
        float VerticalSpeed = 0;
        bool Seated = false;
    };

    void Slide_Soldier(Soldier &soldier, Vector3 move) const;
    void Tick();

    std::reference_wrapper<const HeadlessCollision> Collision;
    std::map<int, Actor> Soldiers;
    Clock::duration TimeAccumulated = Clock::duration::zero();
};

#endif // HEADLESS_SIMULATION_H
