// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_level.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "aabox.h"
#include "bounded_chunk_reader.h"
#include "colmath.h"
#include "combatchunkid.h"
#include "lineseg.h"
#include "playertype.h"

namespace {

// SaveGameManager, CombatSaveLoadClass and GameObjManager chunk IDs.
constexpr uint32 LevelData = 1011991649;
constexpr uint32 GameObjectManager = 916991654;
constexpr uint32 GameObjects = 916991653;
constexpr uint32 CombatSpawners = 916991657;
constexpr uint32 SpawnerData = 1014991133;
constexpr uint32 SpawnerVariables = 1014991054;
constexpr uint32 SpawnerDefinitionParent = 1013991542;
constexpr uint32 SpawnerDefinitionVariables = 1013991543;
constexpr uint32 DefinitionObjects = 0x101;
constexpr uint32 DefinitionVariables = 0x100;
constexpr uint32 FactoryData = 0x00100101;
constexpr uint32 BuildingVariables = 207011121;
constexpr uint32 DamageableVariables = 207011214;
constexpr uint32 GameObjectVariables = 910991407;

// PhysicsSceneClass and AABTreeCullSystemClass chunk IDs.
constexpr uint32 StaticDataSystem = CHUNKID_WWPHYS_BEGIN;
constexpr uint32 StaticObjectsSystem = CHUNKID_WWPHYS_BEGIN + 1;
constexpr uint32 StaticDataScene = 0x04433220;
constexpr uint32 StaticObjectsScene = 0x06090609;
constexpr uint32 StaticTree = 0x4500;
constexpr uint32 StaticTreeData = 0x104;
constexpr uint32 TreeVersion = 1;
constexpr uint32 TreeNode = 0x101;
constexpr uint32 TreeNodeInfo = 0x102;
constexpr uint32 TreeNodeVariables = 0x104;
constexpr uint32 StaticObjectList = 0x770100;
constexpr uint32 StaticObjectRecord = 0x770101;
constexpr uint32 StaticObjectLink = 0x770102;
constexpr uint32 ObjectNodeIndex = 0x200;
// PhysClass state and RenderObjPersistFactoryClass model reference.
constexpr uint32 PhysVariables = 0x00660055;
constexpr uint32 ModelVariables = 0x00555040;

using LevelReader = BoundedChunkReader;

struct SavedBox {
    IOVector3Struct Center;
    IOVector3Struct Extent;
    uint32 Children;
};
static_assert(sizeof(SavedBox) == 28, "Unexpected AAB tree node layout");

bool Is_Finite(const IOVector3Struct &value)
{
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}

Vector3 Vector(const IOVector3Struct &value)
{
    return Vector3(value.X, value.Y, value.Z);
}

Matrix3D Read_Transform(LevelReader &reader)
{
    auto matrix = reader.Value<std::array<float, 12>>();
    reader.Require(std::all_of(matrix.begin(), matrix.end(), [](float item) { return std::isfinite(item); }),
        "Non-finite object transform");
    return Matrix3D(matrix.data());
}

struct SpawnDefinition {
    int PlayerType = PLAYERTYPE_NEUTRAL;
    bool Primary = false;
    bool SoldierStartup = false;
};

void Load_Spawn_Definitions(const GameDataFile &data, std::map<int, SpawnDefinition> &definitions)
{
    LevelReader reader(data);
    reader.In_Chunk(CHUNKID_SAVELOAD_DEFMGR, [&] {
        reader.In_Chunk(DefinitionObjects, [&] {
            reader.Each_Chunk([&](uint32 id) {
                if (id != CHUNKID_SPAWNER_DEF) {
                    return;
                }
                int definition_id = 0;
                SpawnDefinition definition;
                reader.In_Chunk(FactoryData, [&] {
                    reader.Each_Chunk([&](uint32 part) {
                        if (part == SpawnerDefinitionParent) {
                            reader.In_Chunk(DefinitionVariables, [&] {
                                reader.Each_Micro([&](uint32 field) {
                                    if (field == 1) { // DefinitionClass::VARID_INSTANCEID
                                        definition_id = reader.Value<sint32>();
                                    }
                                });
                            });
                        } else if (part == SpawnerDefinitionVariables) {
                            reader.Each_Micro([&](uint32 field) {
                                switch (field) {
                                case 3: definition.PlayerType = reader.Value<sint32>(); break;
                                case 9: definition.Primary = reader.Value<uint8>() != 0; break;
                                case 15: definition.SoldierStartup = reader.Value<uint8>() != 0; break;
                                }
                            });
                        }
                    });
                });
                reader.Require(definition_id > 0, "Spawner definition is missing its ID");
                definitions[definition_id] = definition;
            });
        });
    });
}

HeadlessLevel::Spawner Read_Spawner(LevelReader &reader, const std::map<int, SpawnDefinition> &definitions)
{
    HeadlessLevel::Spawner spawner;
    bool has_transform = false;
    reader.In_Chunk(SpawnerVariables, [&] {
        reader.Each_Micro([&](uint32 field) {
            switch (field) {
            case 1: spawner.Id = reader.Value<sint32>(); break;
            case 2:
                spawner.Transform = Read_Transform(reader);
                has_transform = true;
                break;
            case 3: spawner.DefinitionId = reader.Value<sint32>(); break;
            case 6: spawner.Enabled = reader.Value<uint8>() != 0; break;
            }
        });
    });
    const auto definition = definitions.find(spawner.DefinitionId);
    reader.Require(has_transform && definition != definitions.end(),
        "Missing spawn transform or spawner definition " + std::to_string(spawner.DefinitionId));
    spawner.PlayerType = definition->second.PlayerType;
    spawner.Primary = definition->second.Primary;
    spawner.SoldierStartup = definition->second.SoldierStartup;
    return spawner;
}

bool Is_Building(uint32 factory)
{
    switch (factory) {
    case CHUNKID_GAME_OBJECT_BUILDING:
    case CHUNKID_GAME_OBJECT_REFINERY:
    case CHUNKID_GAME_OBJECT_POWERPLANT:
    case CHUNKID_GAME_OBJECT_SOLDIER_FACTORY:
    case CHUNKID_GAME_OBJECT_VEHICLE_FACTORY:
    case CHUNKID_GAME_OBJECT_AIRSTRIP:
    case CHUNKID_GAME_OBJECT_WARFACTORY:
    case CHUNKID_GAME_OBJECT_COMCENTER:
    case CHUNKID_GAME_OBJECT_REPAIR_BAY:
        return true;
    default:
        return false;
    }
}

void Read_Building_Parts(LevelReader &reader, HeadlessLevel::Building &building, bool &has_position,
    bool &has_team, int depth)
{
    reader.Require(depth < 24, "Building inheritance chunks are nested too deeply");
    reader.Each_Chunk([&](uint32 part) {
        if (part == GameObjectVariables || part == BuildingVariables || part == DamageableVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (part == GameObjectVariables && field == 2) {
                    building.DefinitionId = reader.Value<sint32>();
                } else if (part == GameObjectVariables && field == 3) {
                    building.Id = reader.Value<sint32>();
                } else if (part == BuildingVariables && field == 1) {
                    const auto position = reader.Value<IOVector3Struct>();
                    reader.Require(Is_Finite(position), "Non-finite building position");
                    building.Position = Vector(position);
                    has_position = true;
                } else if (part == BuildingVariables && field == 2) {
                    building.Powered = reader.Value<uint8>() != 0;
                } else if (part == DamageableVariables && field == 1) {
                    building.PlayerType = reader.Value<sint32>();
                    has_team = true;
                }
            });
        } else if (reader.Has_Children()) {
            Read_Building_Parts(reader, building, has_position, has_team, depth + 1);
        }
    });
}

void Read_Static_Parts(LevelReader &reader, HeadlessLevel::StaticObject &object, int depth)
{
    reader.Require(depth < 24, "Static object chunks are nested too deeply");
    reader.Each_Chunk([&](uint32 part) {
        if (part == PhysVariables || part == ModelVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (part == PhysVariables && field == 3) {
                    object.Flags = reader.Value<uint32>();
                } else if (part == PhysVariables && field == 7) {
                    object.InstanceId = reader.Value<uint32>();
                } else if (part == ModelVariables && field == 1) {
                    object.AssetName = reader.Read_String();
                } else if (part == ModelVariables && field == 2) {
                    object.Transform = Read_Transform(reader);
                }
            });
        } else if (reader.Has_Children()) {
            Read_Static_Parts(reader, object, depth + 1);
        }
    });
}

} // namespace

HeadlessLevel HeadlessLevel::Load(const GameDataFile &static_data, const GameDataFile &dynamic_data,
    const GameDataFile &definitions, const std::optional<GameDataFile> &map_definitions)
{
    HeadlessLevel level;
    level.Load_Static(static_data);
    level.Load_Dynamic(dynamic_data, definitions, map_definitions);
    return level;
}

void HeadlessLevel::Load_Static(const GameDataFile &data)
{
    LevelReader reader(data);
    std::vector<uint32> links;
    reader.Each_Chunk([&](uint32 system) {
        if (system == StaticDataSystem) {
            reader.Require(Nodes.empty(), "Duplicate static data subsystem");
            reader.In_Chunk(StaticDataScene, [&] {
                reader.In_Chunk(StaticTree, [&] {
                    reader.In_Chunk(StaticTreeData, [&] {
                        bool version_valid = false;
                        // The original loader indexes nodes in preorder, front before back.
                        std::vector<std::pair<std::size_t, bool>> pending_children;
                        reader.Each_Chunk([&](uint32 id) {
                            if (id == TreeVersion) {
                                reader.Require(!version_valid && Nodes.empty() && reader.Value<uint32>() == 0x10000,
                                    "Unsupported AAB tree version");
                                version_valid = true;
                                return;
                            }
                            if (id != TreeNode) {
                                return;
                            }
                            reader.Require(version_valid && (Nodes.empty() || !pending_children.empty()),
                                "Unexpected AAB tree node");
                            std::optional<SavedBox> saved;
                            reader.Each_Chunk([&](uint32 part) {
                                if (part == TreeNodeInfo) {
                                    saved = reader.Value<SavedBox>();
                                } else if (part == TreeNodeVariables) {
                                    reader.Each_Micro([&](uint32 field) {
                                        if (field == 0) {
                                            saved = reader.Value<SavedBox>();
                                        }
                                    });
                                }
                            });
                            reader.Require(saved && Is_Finite(saved->Center) && Is_Finite(saved->Extent) &&
                                saved->Extent.X >= 0 && saved->Extent.Y >= 0 && saved->Extent.Z >= 0 &&
                                saved->Children <= 3, "Invalid AAB tree bounds or child flags");
                            const auto index = Nodes.size();
                            if (!pending_children.empty()) {
                                const auto [parent, front] = pending_children.back();
                                pending_children.pop_back();
                                (front ? Nodes[parent].Front : Nodes[parent].Back) = index;
                            }
                            Node node;
                            node.Box = {Vector(saved->Center), Vector(saved->Extent)};
                            Nodes.push_back(std::move(node));
                            if (saved->Children & 2) { pending_children.emplace_back(index, false); }
                            if (saved->Children & 1) { pending_children.emplace_back(index, true); }
                        });
                        reader.Require(version_valid && !Nodes.empty() && pending_children.empty(), "Incomplete AAB tree");
                    });
                });
            });
        } else if (system == StaticObjectsSystem) {
            reader.In_Chunk(StaticObjectsScene, [&] {
                reader.In_Chunk(StaticObjectList, [&] {
                    bool expecting_link = false;
                    reader.Each_Chunk([&](uint32 id) {
                        if (id == StaticObjectRecord) {
                            reader.Require(!expecting_link, "Static object has no AAB tree link");
                            StaticObject object;
                            reader.Each_Chunk([&](uint32 factory) {
                                reader.Require(object.FactoryId == 0, "Static object has multiple factories");
                                object.FactoryId = factory;
                                Read_Static_Parts(reader, object, 0);
                            });
                            reader.Require(object.FactoryId != 0, "Static object is missing its factory");
                            LevelStaticObjects.push_back(std::move(object));
                            expecting_link = true;
                        } else if (id == StaticObjectLink) {
                            reader.Require(expecting_link, "AAB tree link has no static object");
                            reader.In_Chunk(ObjectNodeIndex, [&] { links.push_back(reader.Value<uint32>()); });
                            expecting_link = false;
                        }
                    });
                    reader.Require(!expecting_link, "Last static object has no AAB tree link");
                });
            });
        }
    });
    reader.Require(!Nodes.empty(), "Missing static AAB tree");
    for (std::size_t object = 0; object < links.size(); ++object) {
        const auto index = links[object];
        reader.Require(index < Nodes.size(), "Static object links to an invalid AAB tree node");
        Nodes[index].Objects.push_back(object);
    }
}

void HeadlessLevel::Load_Dynamic(const GameDataFile &data, const GameDataFile &definitions,
    const std::optional<GameDataFile> &map_definitions)
{
    std::map<int, SpawnDefinition> spawner_definitions;
    Load_Spawn_Definitions(definitions, spawner_definitions);
    if (map_definitions) {
        Load_Spawn_Definitions(*map_definitions, spawner_definitions);
    }
    LevelReader reader(data);
    reader.In_Chunk(LevelData, [&] {
        reader.In_Chunk(CHUNKID_COMBAT, [&] {
            reader.Each_Chunk([&](uint32 part) {
                if (part == CombatSpawners) {
                    reader.Each_Chunk([&](uint32 id) {
                        if (id == SpawnerData) {
                            LevelSpawners.push_back(Read_Spawner(reader, spawner_definitions));
                        }
                    });
                } else if (part == GameObjectManager) {
                    reader.In_Chunk(GameObjects, [&] {
                        reader.Each_Chunk([&](uint32 factory) {
                            if (!Is_Building(factory)) {
                                return;
                            }
                            Building building;
                            building.FactoryId = factory;
                            bool has_position = false;
                            bool has_team = false;
                            reader.In_Chunk(FactoryData, [&] {
                                Read_Building_Parts(reader, building, has_position, has_team, 0);
                            });
                            reader.Require(has_position && has_team && building.DefinitionId > 0,
                                "Building is missing serialized gameplay state");
                            LevelBuildings.push_back(building);
                        });
                    });
                }
            });
        });
    });
}

bool HeadlessLevel::Bounds::Contains(const Vector3 &point) const
{
    return std::abs(point.X - Center.X) <= Extent.X && std::abs(point.Y - Center.Y) <= Extent.Y &&
        std::abs(point.Z - Center.Z) <= Extent.Z;
}

std::size_t HeadlessLevel::Count_Player_Spawns(int player_type) const
{
    return std::count_if(LevelSpawners.begin(), LevelSpawners.end(), [&](const Spawner &spawn) {
        return spawn.SoldierStartup && !spawn.Primary && spawn.PlayerType == player_type;
    });
}

std::vector<std::size_t> HeadlessLevel::Find_Static_Candidates(const Vector3 &point) const
{
    return Collect_Static_Candidates([&](const Bounds &box) { return box.Contains(point); });
}

std::vector<std::size_t> HeadlessLevel::Find_Static_Candidates(const Vector3 &start, const Vector3 &end) const
{
    return Find_Static_Candidates(start, end, Vector3(0, 0, 0));
}

std::vector<std::size_t> HeadlessLevel::Find_Static_Candidates(const Vector3 &start, const Vector3 &end,
    const Vector3 &extent) const
{
    const LineSegClass ray(start, end);
    return Collect_Static_Candidates([&](const Bounds &box) {
        const Bounds expanded{box.Center, box.Extent + extent};
        CastResultStruct result;
        return expanded.Contains(end) ||
            CollisionMath::Collide(ray, AABoxClass(expanded.Center, expanded.Extent), &result);
    });
}

template<class Intersects> std::vector<std::size_t> HeadlessLevel::Collect_Static_Candidates(Intersects intersects) const
{
    std::vector<std::size_t> objects;
    std::vector<std::size_t> pending{0};
    while (!pending.empty()) {
        const auto &node = Nodes[pending.back()];
        pending.pop_back();
        if (intersects(node.Box)) {
            objects.insert(objects.end(), node.Objects.begin(), node.Objects.end());
            if (node.Front != NoNode) { pending.push_back(node.Front); }
            if (node.Back != NoNode) { pending.push_back(node.Back); }
        }
    }
    return objects;
}
