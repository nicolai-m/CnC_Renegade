// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_simulation.h"

#include <algorithm>
#include <cmath>

#include "aabox.h"
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

AABoxClass Soldier_Box(const Vector3 &feet)
{
    // Use the soldier debug box until character-specific hulls are loaded.
    const Vector3 extent(0.3f, 0.3f, 1.0f);
    return AABoxClass(feet + Vector3(0, 0, extent.Z), extent);
}

} // namespace

HeadlessSimulation::HeadlessSimulation(const HeadlessCollision &collision) : Collision(collision)
{
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
    const auto hit = Collision.get().Sweep_Box(Soldier_Box(position), Vector3(0, 0, -drop), COLLISION_TYPE_PHYSICAL);
    if (!hit || hit->StartsInside || hit->Normal.Z < WalkableNormalZ) {
        return std::nullopt;
    }
    return position - Vector3(0, 0, std::max(0.0f, drop * hit->Fraction - ContactClearance));
}

void HeadlessSimulation::Slide_Soldier(Soldier &soldier, Vector3 move) const
{
    for (int contact = 0; contact < MaxSlideContacts && move.Length2() > MinMove * MinMove; ++contact) {
        const auto hit = Collision.get().Sweep_Box(Soldier_Box(soldier.Position), move, COLLISION_TYPE_PHYSICAL);
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
    for (auto &entry : Soldiers) {
        auto &actor = entry.second;
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
