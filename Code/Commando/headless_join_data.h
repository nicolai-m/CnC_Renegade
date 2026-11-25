// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_JOIN_DATA_H
#define HEADLESS_JOIN_DATA_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "headless_collision.h"

struct HeadlessSpawnWeapon {
    int DefinitionId;
    int Rounds; // Total rounds. The original protocol sends -1 for unlimited ammo.
    float KeyNumber;
};

struct HeadlessSoldierPreset {
    int DefinitionId = 0;
    std::string Model;
    int Health = 0;
    int Shield = 0;
    unsigned ShieldType = 0;
    std::vector<HeadlessSpawnWeapon> Weapons;
    int StartingWeapon = 0;
};

struct HeadlessJoinData {
    std::uint32_t VersionKey = 0;
    std::uint32_t ExeCRC = 0;
    std::uint32_t StringsCRC = 0;
    unsigned ArmorTypes = 0;
    std::array<HeadlessSoldierPreset, 2> Soldiers; // Nod, GDI.
};

HeadlessJoinData Load_Headless_Join_Data(std::uint32_t client_build, unsigned armor_types,
    const GameDataFile &strings, const GameDataFile &definitions, const std::optional<GameDataFile> &map_definitions,
    const HeadlessCollision::FindAsset &find_core_asset);

#endif // HEADLESS_JOIN_DATA_H
