// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_SIMULATION_H
#define HEADLESS_SIMULATION_H

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "matrix3d.h"
#include "vector2.h"
#include "vector3.h"

class HeadlessCollision;
class AABoxClass;
class OBBoxClass;

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

    struct Vehicle {
        Matrix3D Transform;
        Vector3 Velocity{0, 0, 0};
        float YawRate = 0;
        int Driver = 0;
        Vector2 Input{0, 0}; // Throttle and left steering.
    };

    explicit HeadlessSimulation(const HeadlessCollision &collision);

    bool Spawn_Soldier(int player_id, int player_type);
    void Remove_Soldier(int player_id);
    const Soldier *Find_Soldier(int player_id) const;
    static AABoxClass Soldier_Bounds(const Vector3 &feet);
    const HeadlessCollision &Map_Collision() const { return Collision.get(); }

    // World-space movement axes in [-1, 1], held until the next input.
    bool Set_Move_Input(int player_id, const Vector2 &direction);
    const Vehicle &Vehicle_State(std::size_t index) const { return Vehicles[index].State; }
    void Set_Vehicle_Driver(std::size_t index, int player_id, const Vector3 &position);
    void Set_Drive_Input(std::size_t index, const Vector2 &input);
    std::optional<Vector3> Find_Vehicle_Exit(std::size_t index) const;
    void Advance(Clock::duration elapsed);

private:
    struct Actor {
        Soldier State;
        Vector2 MoveInput{0, 0};
        float VerticalSpeed = 0;
        bool Seated = false;
    };

    struct VehicleActor {
        Vehicle State;
        float Speed = 0;
        bool Settled = false;
    };

    OBBoxClass Vehicle_Box(std::size_t index, const Matrix3D &pose) const;
    std::optional<Matrix3D> Find_Vehicle_Ground(std::size_t index, Matrix3D pose, float drop) const;
    void Tick_Vehicle(std::size_t index, VehicleActor &actor);
    std::optional<Vector3> Find_Stand_Position(const Vector3 &position, float drop) const;
    void Slide_Soldier(Soldier &soldier, Vector3 move) const;
    void Tick();

    std::reference_wrapper<const HeadlessCollision> Collision;
    std::map<int, Actor> Soldiers;
    std::vector<VehicleActor> Vehicles;
    Clock::duration TimeAccumulated = Clock::duration::zero();
};

#endif // HEADLESS_SIMULATION_H
