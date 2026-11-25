// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_simulation.h"

#include <algorithm>
#include <cmath>

#include "aabox.h"
#include "colmath.h"
#include "coltype.h"
#include "headless_collision.h"
#include "playertype.h"

namespace {

constexpr auto MaxCatchUp = HeadlessSimulation::TickInterval * 5;
constexpr float TickSeconds = std::chrono::duration<float>(HeadlessSimulation::TickInterval).count();
constexpr float MoveSpeed = 4.0f;
constexpr float Gravity = 9.8f;
constexpr float SpawnDrop = 5.0f;
constexpr float GroundProbeDistance = 0.10f;
constexpr float WalkableNormalZ = 0.70710678f; // 45 degrees, the default in Phys3Class.
constexpr float ContactClearance = 0.002f;
constexpr float MinMove = 0.00001f;
constexpr int MaxSlideContacts = 4;
constexpr unsigned VehicleMask = COLLISION_TYPE_PHYSICAL | COLLISION_TYPE_VEHICLE;
constexpr float VehicleClearance = 0.03f;
constexpr float VehicleStepHeight = 0.23f;
constexpr float VehicleGroundDrop = 0.35f;
constexpr float InitialVehicleLift = 0.55f;
constexpr float GroundVehicleSpeed = 15.0f;
constexpr float TrackedVehicleSpeed = 10.0f;
constexpr float ReverseSpeed = 6.0f;
constexpr float VehicleAcceleration = 8.0f;
constexpr float VehicleBraking = 12.0f;
constexpr float WheeledTurnRate = 1.65f;
constexpr float TrackedTurnRate = 1.35f;
constexpr float FullSteeringSpeed = 4.0f;

} // namespace

AABoxClass HeadlessSimulation::Soldier_Bounds(const Vector3 &feet)
{
    // Use the soldier debug box until character-specific hulls are loaded.
    const Vector3 extent(0.3f, 0.3f, 1.0f);
    return AABoxClass(feet + Vector3(0, 0, extent.Z), extent);
}

HeadlessSimulation::HeadlessSimulation(const HeadlessCollision &collision) : Collision(collision)
{
    Vehicles.reserve(collision.Level().Vehicles().size());
    for (const auto &vehicle : collision.Level().Vehicles()) {
        Vehicles.push_back({{vehicle.Transform}});
    }
}

bool HeadlessSimulation::Spawn_Soldier(int player_id, int player_type)
{
    if (player_id <= 0 || (player_type != PLAYERTYPE_GDI && player_type != PLAYERTYPE_NOD) ||
        Soldiers.find(player_id) != Soldiers.end()) {
        return false;
    }

    for (const auto &spawner : Collision.get().Level().Spawners()) {
        if (!spawner.Enabled || !spawner.SoldierStartup || spawner.Primary || spawner.PlayerType != player_type) {
            continue;
        }
        if (const auto position = Find_Stand_Position(spawner.Transform.Get_Translation(), SpawnDrop)) {
            const auto forward = spawner.Transform.Get_X_Vector();
            Soldiers.emplace(player_id, Actor{Soldier{player_type, spawner.Id, *position, Vector3(0, 0, 0),
                std::atan2(forward.Y, forward.X), true}});
            return true;
        }
    }
    return false;
}

void HeadlessSimulation::Remove_Soldier(int player_id)
{
    Soldiers.erase(player_id);
}

const HeadlessSimulation::Soldier *HeadlessSimulation::Find_Soldier(int player_id) const
{
    const auto found = Soldiers.find(player_id);
    return found == Soldiers.end() ? nullptr : &found->second.State;
}

bool HeadlessSimulation::Set_Move_Input(int player_id, const Vector2 &direction)
{
    const auto found = Soldiers.find(player_id);
    if (found == Soldiers.end() || !std::isfinite(direction.X) || !std::isfinite(direction.Y) ||
        std::abs(direction.X) > 1.0f || std::abs(direction.Y) > 1.0f) {
        return false;
    }
    auto &input = found->second.MoveInput;
    input = direction;
    if (input.Length2() > 1.0f) {
        input.Normalize();
    }
    return true;
}

void HeadlessSimulation::Set_Vehicle_Driver(std::size_t index, int player_id, const Vector3 &position)
{
    auto &vehicle = Vehicles[index];
    const bool seated = player_id != 0;
    auto &soldier = Soldiers.at(seated ? player_id : vehicle.State.Driver);
    soldier.State.Position = position;
    soldier.State.Velocity.Set(0, 0, 0);
    soldier.State.Grounded = !seated;
    soldier.MoveInput.Set(0, 0);
    soldier.VerticalSpeed = 0;
    soldier.Seated = seated;
    vehicle.State.Driver = player_id;
    vehicle.State.Input.Set(0, 0);
    vehicle.State.Velocity.Set(0, 0, 0);
    vehicle.State.YawRate = 0;
    vehicle.Speed = 0;
}

void HeadlessSimulation::Set_Drive_Input(std::size_t index, const Vector2 &input)
{
    Vehicles[index].State.Input = input;
}

OBBoxClass HeadlessSimulation::Vehicle_Box(std::size_t index, const Matrix3D &pose) const
{
    OBBoxClass world;
    OBBoxClass::Transform(pose, Collision.get().Vehicle_Hull(index), &world);
    return world;
}

std::optional<Vector3> HeadlessSimulation::Find_Vehicle_Exit(std::size_t index) const
{
    const auto &collision = Collision.get();
    const auto &pose = Vehicles[index].State.Transform;
    const auto hull = Vehicle_Box(index, pose);
    const Vector3 still(0, 0, 0);
    const Vector3 eye(0, 0, 0.8f);
    for (const auto &offset : collision.Level().Vehicles()[index].Exits) {
        const auto feet = Find_Stand_Position(pose * offset + Vector3(0, 0, 0.5f), 2.0f);
        CastResultStruct overlap;
        if (feet && !CollisionMath::Collide(hull, still, Soldier_Bounds(*feet), still, &overlap) &&
            !collision.Cast_Ray(pose.Get_Translation() + eye, *feet + eye, COLLISION_TYPE_PHYSICAL)) {
            return feet;
        }
    }
    return std::nullopt;
}

std::optional<Matrix3D> HeadlessSimulation::Find_Vehicle_Ground(std::size_t index, Matrix3D pose, float drop) const
{
    const auto ground = Collision.get().Sweep_Box(Vehicle_Box(index, pose), Vector3(0, 0, -drop), VehicleMask);
    if (!ground || ground->StartsInside || ground->Normal.Z < WalkableNormalZ) {
        return std::nullopt;
    }
    pose.Set_Translation(pose.Get_Translation() - Vector3(0, 0, drop * ground->Fraction - VehicleClearance));
    return pose;
}

void HeadlessSimulation::Tick_Vehicle(std::size_t index, VehicleActor &actor)
{
    auto &vehicle = actor.State;
    vehicle.Velocity.Set(0, 0, 0);
    vehicle.YawRate = 0;
    if (vehicle.Driver == 0 || (vehicle.Input == Vector2(0, 0) && actor.Speed == 0)) {
        return;
    }

    auto &soldier = Soldiers.at(vehicle.Driver).State;
    const auto &collision = Collision.get();
    const bool tracked = collision.Level().Vehicles()[index].Tracked;
    if (!actor.Settled) {
        auto ground = Find_Vehicle_Ground(index, vehicle.Transform, VehicleGroundDrop);
        if (!ground) {
            // Saved poses can put the chassis partly through the ground at startup.
            auto raised = vehicle.Transform;
            raised.Set_Translation(vehicle.Transform.Get_Translation() + Vector3(0, 0, InitialVehicleLift));
            ground = Find_Vehicle_Ground(index, raised, InitialVehicleLift + VehicleGroundDrop);
        }
        if (!ground) {
            return;
        }
        vehicle.Transform = *ground;
        soldier.Position = ground->Get_Translation();
        actor.Settled = true;
    }

    const auto origin = vehicle.Transform.Get_Translation();
    const float throttle = vehicle.Input.X;
    const float target_speed = throttle * (throttle < 0 ? ReverseSpeed : tracked ? TrackedVehicleSpeed : GroundVehicleSpeed);
    const bool braking = std::abs(target_speed) < std::abs(actor.Speed) || actor.Speed * target_speed < 0;
    const float speed_step = (braking ? VehicleBraking : VehicleAcceleration) * TickSeconds;
    actor.Speed += std::clamp(target_speed - actor.Speed, -speed_step, speed_step);
    const float steering_scale = tracked ? 1.0f : std::clamp(actor.Speed / FullSteeringSpeed, -1.0f, 1.0f);
    float turn = vehicle.Input.Y * steering_scale * (tracked ? TrackedTurnRate : WheeledTurnRate) * TickSeconds;

    const Vector3 lift(0, 0, VehicleStepHeight);
    const auto ceiling = collision.Sweep_Box(Vehicle_Box(index, vehicle.Transform), lift, VehicleMask);
    const float lift_height = ceiling ? std::max(0.0f, lift.Z * ceiling->Fraction - VehicleClearance) : lift.Z;
    auto pose = vehicle.Transform;
    pose.Set_Translation(origin + Vector3(0, 0, lift_height));
    if (turn != 0) {
        pose.In_Place_Pre_Rotate_Z(turn);
        if (collision.Sweep_Box(Vehicle_Box(index, pose), Vector3(0, 0, 0), VehicleMask)) {
            pose = vehicle.Transform;
            pose.Set_Translation(origin + Vector3(0, 0, lift_height));
            turn = 0;
        }
    }

    const auto forward = pose.Get_X_Vector();
    Vector3 travel = (actor.Speed * TickSeconds) * Vector3(forward.X, forward.Y, 0);
    if (travel.Length2() > MinMove * MinMove) {
        if (const auto obstacle = collision.Sweep_Box(Vehicle_Box(index, pose), travel, VehicleMask)) {
            const float toward = -Vector3::Dot_Product(obstacle->Normal, travel);
            travel *= toward > MinMove ? std::max(0.0f, obstacle->Fraction - VehicleClearance / toward) : 0.0f;
            actor.Speed = 0;
        }
    }
    pose.Set_Translation(pose.Get_Translation() + travel);
    const auto ground = Find_Vehicle_Ground(index, pose, lift_height + VehicleGroundDrop);
    if (!ground) {
        actor.Speed = 0;
        return;
    }
    vehicle.Transform = *ground;
    soldier.Position = ground->Get_Translation();
    vehicle.Velocity = (soldier.Position - origin) / TickSeconds;
    vehicle.YawRate = turn / TickSeconds;
}

void HeadlessSimulation::Advance(Clock::duration elapsed)
{
    if (elapsed <= Clock::duration::zero()) {
        return;
    }
    // Bound catch-up after a stalled server frame and preserve sub-tick time.
    TimeAccumulated += std::min(elapsed, std::chrono::duration_cast<Clock::duration>(MaxCatchUp));
    while (TimeAccumulated >= TickInterval) {
        Tick();
        TimeAccumulated -= TickInterval;
    }
}

std::optional<Vector3> HeadlessSimulation::Find_Stand_Position(const Vector3 &position, float drop) const
{
    const auto hit = Collision.get().Sweep_Box(Soldier_Bounds(position), Vector3(0, 0, -drop), COLLISION_TYPE_PHYSICAL);
    if (!hit || hit->StartsInside || hit->Normal.Z < WalkableNormalZ) {
        return std::nullopt;
    }
    return position - Vector3(0, 0, std::max(0.0f, drop * hit->Fraction - ContactClearance));
}

void HeadlessSimulation::Slide_Soldier(Soldier &soldier, Vector3 move) const
{
    for (int contact = 0; contact < MaxSlideContacts && move.Length2() > MinMove * MinMove; ++contact) {
        const auto hit = Collision.get().Sweep_Box(Soldier_Bounds(soldier.Position), move, COLLISION_TYPE_PHYSICAL);
        if (!hit) {
            soldier.Position += move;
            return;
        }
        if (hit->StartsInside) {
            return;
        }

        const float toward_surface = -Vector3::Dot_Product(move, hit->Normal);
        if (toward_surface <= MinMove) {
            return;
        }
        // Keep a small clearance measured along the blocking surface normal.
        const float travel = std::max(0.0f, hit->Fraction - ContactClearance / toward_surface);
        soldier.Position += travel * move;
        move = (1.0f - travel) * (move + toward_surface * hit->Normal);
    }
}

void HeadlessSimulation::Tick()
{
    for (std::size_t index = 0; index < Vehicles.size(); ++index) {
        Tick_Vehicle(index, Vehicles[index]);
    }
    for (auto &entry : Soldiers) {
        auto &actor = entry.second;
        if (actor.Seated) {
            continue;
        }
        auto &soldier = actor.State;
        const auto previous_position = soldier.Position;
        actor.VerticalSpeed -= Gravity * TickSeconds;
        const Vector3 velocity(actor.MoveInput.X * MoveSpeed, actor.MoveInput.Y * MoveSpeed, actor.VerticalSpeed);
        Slide_Soldier(soldier, TickSeconds * velocity);

        const auto ground = Find_Stand_Position(soldier.Position, GroundProbeDistance);
        soldier.Grounded = ground.has_value();
        if (ground) {
            soldier.Position = *ground;
            actor.VerticalSpeed = 0;
        }
        soldier.Velocity = (soldier.Position - previous_position) / TickSeconds;
    }
}
