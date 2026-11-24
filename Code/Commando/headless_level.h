// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_LEVEL_H
#define HEADLESS_LEVEL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "matrix3d.h"
#include "mixarchive.h"

class HeadlessLevel {
public:
    struct Bounds {
        Vector3 Center;
        Vector3 Extent;
        bool Contains(const Vector3 &point) const;
    };

    struct Spawner {
        int Id = 0;
        int DefinitionId = 0;
        int PlayerType = 0;
        bool Enabled = true;
        bool Primary = false;
        bool SoldierStartup = false;
        Matrix3D Transform{true};
    };

    struct Building {
        std::uint32_t FactoryId = 0;
        int Id = 0;
        int DefinitionId = 0;
        int PlayerType = 0;
        bool Powered = true;
        Vector3 Position{0, 0, 0};
    };

    struct StaticObject {
        std::uint32_t FactoryId = 0;
        std::uint32_t InstanceId = 0;
        std::uint32_t Flags = 0;
        std::string AssetName;
        Matrix3D Transform{true};
    };

    static HeadlessLevel Load(const GameDataFile &static_data, const GameDataFile &dynamic_data,
        const GameDataFile &definitions, const std::optional<GameDataFile> &map_definitions);

    const Bounds &World_Bounds() const { return Nodes.front().Box; }
    std::size_t Node_Count() const { return Nodes.size(); }
    const std::vector<StaticObject> &Static_Objects() const { return LevelStaticObjects; }
    const std::vector<Spawner> &Spawners() const { return LevelSpawners; }
    const std::vector<Building> &Buildings() const { return LevelBuildings; }
    std::size_t Count_Player_Spawns(int player_type) const;

    // Returns indices into Static_Objects() for the intersected tree nodes.
    // It does not test individual mesh geometry or collision flags.
    std::vector<std::size_t> Find_Static_Candidates(const Vector3 &point) const;
    std::vector<std::size_t> Find_Static_Candidates(const Vector3 &start, const Vector3 &end) const;
    std::vector<std::size_t> Find_Static_Candidates(const Vector3 &start, const Vector3 &end,
        const Vector3 &extent) const;

private:
    HeadlessLevel() = default;

    enum : std::size_t { NoNode = static_cast<std::size_t>(-1) };
    struct Node {
        Bounds Box;
        std::size_t Front = NoNode;
        std::size_t Back = NoNode;
        std::vector<std::size_t> Objects;
    };

    void Load_Static(const GameDataFile &data);
    void Load_Dynamic(const GameDataFile &data, const GameDataFile &definitions,
        const std::optional<GameDataFile> &map_definitions);
    template<class Intersects> std::vector<std::size_t> Collect_Static_Candidates(Intersects intersects) const;

    std::vector<Node> Nodes;
    std::vector<StaticObject> LevelStaticObjects;
    std::vector<Spawner> LevelSpawners;
    std::vector<Building> LevelBuildings;
};

#endif // HEADLESS_LEVEL_H
