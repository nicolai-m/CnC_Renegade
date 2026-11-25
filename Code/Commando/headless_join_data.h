// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_JOIN_DATA_H
#define HEADLESS_JOIN_DATA_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "headless_collision.h"

struct HeadlessBulletDefinition {
    float Damage;
    unsigned Warhead;
    float Range;
    float Speed;
    float Spread;
};

struct HeadlessExplosionDefinition {
    int Id = 0;
    float Radius = 0;
    float Damage = 0;
    unsigned Warhead = 0;
    bool Scaled = false;
};

enum class HeadlessChargeTrigger { Remote, Timed, Proximity };

struct HeadlessChargeDefinition {
    HeadlessChargeTrigger Trigger;
    int ObjectPreset;
    int AmmoPreset;
    std::string Model;
    float ThrowSpeed;
    float GravityScale;
    std::array<float, 3> TriggerTimes;
    std::array<float, 3> TriggerRanges;
    float Health;
    float Shield;
    unsigned ShieldType;
    HeadlessExplosionDefinition Explosion;
};

struct HeadlessFireMode {
    float ShotInterval;
    float ChargeSeconds;
    int RoundCost;
    std::optional<HeadlessBulletDefinition> Bullet;
    std::optional<HeadlessChargeDefinition> Charge;
};

struct HeadlessSpawnWeapon {
    int DefinitionId;
    float KeyNumber;
    int ClipSize;
    int ClipRounds;
    int ReserveRounds;
    float ReloadSeconds;
    std::optional<HeadlessFireMode> PrimaryFire;
    std::optional<HeadlessFireMode> SecondaryFire;
    unsigned DetonationMode = 0;

    int Total_Rounds() const;
};

struct HeadlessSoldierPreset {
    int DefinitionId = 0;
    std::string Model;
    float Health = 100;
    float Shield = 0;
    unsigned ShieldType = 0;
    std::vector<HeadlessSpawnWeapon> Weapons;
    int StartingWeapon = 0;
    unsigned Skin = 0;
    float DamagePoints = 0;
    float DeathPoints = 0;
    float BullseyeHeight = 1.2f;
};

struct HeadlessDamageRule {
    float Scale = 1;
    float ShieldAbsorption = 0;
};

struct HeadlessArmorType {
    int SaveId = 0;
    std::vector<HeadlessDamageRule> Warheads;
};

using HeadlessArmorTable = std::vector<HeadlessArmorType>;
unsigned Headless_Armor_Index(const HeadlessArmorTable &armor, int save_id);

struct HeadlessJoinData {
    std::uint32_t VersionKey = 0;
    std::uint32_t ExeCRC = 0;
    std::uint32_t StringsCRC = 0;
    HeadlessArmorTable Armor;
    std::array<HeadlessSoldierPreset, 2> Soldiers; // Nod, GDI.
};

enum class HeadlessSpawnCharacter { Minigunner, Engineer, Technician };

HeadlessJoinData Load_Headless_Join_Data(std::uint32_t client_build, const HeadlessArmorTable &armor,
    const GameDataFile &strings, const GameDataFile &definitions, const std::optional<GameDataFile> &map_definitions,
    const HeadlessCollision::FindAsset &find_core_asset,
    HeadlessSpawnCharacter character = HeadlessSpawnCharacter::Minigunner);

#endif // HEADLESS_JOIN_DATA_H
