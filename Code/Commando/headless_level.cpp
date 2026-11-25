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
#include "wwphysids.h"

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
constexpr uint32 BuildingDefinitionVariables = 207011031;
constexpr uint32 DamageableVariables = 207011214;
constexpr uint32 DamageableDefinitionVariables = 207011206;
constexpr uint32 DefenseVariables = 914991020;
constexpr uint32 AggregateDefinitionVariables = 8281442;
constexpr uint32 GameObjectVariables = 910991407;
constexpr uint32 PhysicalObjectVariables = 910991146;
constexpr uint32 VehicleDefinitionVariables = 930991657;
constexpr uint32 TransitionVariables = 0x11051106;

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
constexpr uint32 DynamicDataScene = 0x00007001;
constexpr uint32 DynamicObjectList = 0x00890100;
constexpr uint32 DynamicObjectRecord = 0x00890101;

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

bool Is_Building(uint32 factory);

struct BuildingDefinition {
    std::string MeshPrefix;
    unsigned MCTSkin = 0;
    std::optional<int> PlayerType;
};

struct VehicleDefinition {
    enum Type { Car, Tank, Bike, Flying, Turret };
    int Kind = Car;
    int Seats = 2; // VehicleGameObjDef default for saves without NumSeats.
    std::vector<OBBoxClass> Entrances;
    std::vector<Vector3> Exits;
};

struct MapDefinitions {
    std::map<int, SpawnDefinition> Spawners;
    std::map<int, BuildingDefinition> Buildings;
    std::map<int, bool> AggregateMCT;
    std::map<int, VehicleDefinition> Vehicles;
};

void Read_Vehicle_Definition(LevelReader &reader, int &definition_id, VehicleDefinition &vehicle, int depth = 0)
{
    reader.Require(depth < 24, "Vehicle definition chunks are nested too deeply");
    reader.Each_Chunk([&](uint32 part) {
        if (part == DefinitionVariables || part == VehicleDefinitionVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (part == DefinitionVariables && field == 1) {
                    definition_id = reader.Value<sint32>();
                } else if (part == VehicleDefinitionVariables && field == 1) {
                    vehicle.Kind = reader.Value<sint32>();
                } else if (part == VehicleDefinitionVariables && field == 32) {
                    vehicle.Seats = reader.Value<sint32>();
                }
            });
        } else if (part == TransitionVariables) {
            // TransitionDataClass stores the basis, center and half extents in vehicle space.
            int type = -1;
            std::optional<OBBoxClass> zone;
            std::optional<Vector3> exit;
            reader.Each_Micro([&](uint32 field) {
                if (field == 1) {
                    type = reader.Value<sint32>();
                } else if (field == 2) {
                    const auto box = reader.Value<std::array<float, 15>>();
                    reader.Require(std::all_of(box.begin(), box.end(), [](float v) { return std::isfinite(v); }) &&
                        std::all_of(box.begin() + 12, box.end(), [](float v) { return v >= 0; }),
                        "Invalid vehicle transition zone");
                    zone.emplace(Vector3(box[9], box[10], box[11]), Vector3(box[12], box[13], box[14]),
                        Matrix3(box[0], box[1], box[2], box[3], box[4], box[5], box[6], box[7], box[8]));
                } else if (field == 4) {
                    exit = Read_Transform(reader).Get_Translation();
                }
            });
            if ((type == 8 || type == 4 || type == 5) && zone) {
                vehicle.Entrances.push_back(*zone); // VEHICLE_ENTER and its legacy aliases.
            } else if ((type == 9 || type == 6 || type == 7) && exit) {
                vehicle.Exits.push_back(*exit); // VEHICLE_EXIT and its legacy aliases.
            }
        } else if (reader.Has_Children()) {
            Read_Vehicle_Definition(reader, definition_id, vehicle, depth + 1);
        }
    });
}

void Read_Building_Definition(LevelReader &reader, int &definition_id, BuildingDefinition &building,
    bool &mct, int depth = 0)
{
    reader.Require(depth < 24, "Building definition chunks are nested too deeply");
    reader.Each_Chunk([&](uint32 id) {
        if (id == DefinitionVariables || id == BuildingDefinitionVariables || id == DamageableDefinitionVariables ||
            id == AggregateDefinitionVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (id == DefinitionVariables && field == 1) {
                    definition_id = reader.Value<sint32>();
                } else if (id == BuildingDefinitionVariables && field == 1) {
                    building.MeshPrefix = reader.Read_String();
                } else if (id == BuildingDefinitionVariables && field == 2) {
                    building.MCTSkin = reader.Value<uint32>();
                } else if (id == DamageableDefinitionVariables && field == 6) {
                    building.PlayerType = reader.Value<sint32>();
                } else if (id == AggregateDefinitionVariables && field == 4) {
                    mct = reader.Value<uint8>() != 0;
                }
            });
        } else if (reader.Has_Children()) {
            Read_Building_Definition(reader, definition_id, building, mct, depth + 1);
        }
    });
}

void Load_Map_Definitions(const GameDataFile &data, MapDefinitions &definitions)
{
    LevelReader reader(data);
    reader.In_Chunk(CHUNKID_SAVELOAD_DEFMGR, [&] {
        reader.In_Chunk(DefinitionObjects, [&] {
            reader.Each_Chunk([&](uint32 id) {
                if (id == CHUNKID_GAME_OBJECT_DEF_VEHICLE) {
                    int definition_id = 0;
                    VehicleDefinition vehicle;
                    reader.In_Chunk(FactoryData, [&] { Read_Vehicle_Definition(reader, definition_id, vehicle); });
                    reader.Require(definition_id > 0 && vehicle.Kind >= VehicleDefinition::Car &&
                        vehicle.Kind <= VehicleDefinition::Turret && vehicle.Seats >= 0 && vehicle.Seats <= 32,
                        "Invalid vehicle definition");
                    definitions.Vehicles[definition_id] = std::move(vehicle);
                    return;
                }
                const bool aggregate = id == PHYSICS_CHUNKID_BUILDINGAGGREGATEDEF;
                if (aggregate || Is_Building(id - 1)) { // A building's definition follows its object factory.
                    int definition_id = 0;
                    BuildingDefinition building;
                    bool mct = false;
                    reader.In_Chunk(FactoryData, [&] { Read_Building_Definition(reader, definition_id, building, mct); });
                    reader.Require(definition_id > 0, "Building definition is missing its ID");
                    if (aggregate) {
                        definitions.AggregateMCT[definition_id] = mct;
                    } else {
                        definitions.Buildings[definition_id] = std::move(building);
                    }
                    return;
                }
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
                definitions.Spawners[definition_id] = definition;
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
        if (part == DefenseVariables) {
            reader.Each_Micro([&](uint32 field) {
                switch (field) {
                case 3: building.Health = reader.Value<float>(); break;
                case 4: building.HealthMax = reader.Value<float>(); break;
                case 5: building.SkinSaveId = reader.Value<sint32>(); break;
                case 6: building.Shield = reader.Value<float>(); break;
                case 8: building.ShieldSaveId = reader.Value<sint32>(); break;
                case 13: building.DamagePoints = reader.Value<float>(); break;
                case 14: building.DeathPoints = reader.Value<float>(); break;
                }
            });
        } else if (part == GameObjectVariables || part == BuildingVariables || part == DamageableVariables) {
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
                } else if (part == BuildingVariables && field == 3) {
                    const auto sphere = reader.Value<std::array<float, 4>>();
                    const auto center = IOVector3Struct{sphere[0], sphere[1], sphere[2]};
                    reader.Require(Is_Finite(center) && std::isfinite(sphere[3]) && sphere[3] >= 0,
                        "Invalid building collection sphere");
                    building.CollectionCenter = Vector(center);
                    building.CollectionRadius = sphere[3];
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

void Read_Physics_Parts(LevelReader &reader, HeadlessLevel::StaticObject &object, int depth,
    uint32 *saved_pointer = nullptr)
{
    reader.Require(depth < 24, "Physics object chunks are nested too deeply");
    reader.Each_Chunk([&](uint32 part) {
        if (part == PhysVariables || part == ModelVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (part == PhysVariables && field == 2 && saved_pointer) {
                    *saved_pointer = reader.Value<uint32>(); // PhysClass's EditableClass base.
                } else if (part == PhysVariables && field == 3) {
                    object.Flags = reader.Value<uint32>();
                } else if (part == PhysVariables && field == 6) {
                    object.DefinitionId = reader.Value<sint32>();
                } else if (part == PhysVariables && field == 7) {
                    object.InstanceId = reader.Value<uint32>();
                } else if (part == ModelVariables && field == 1) {
                    object.AssetName = reader.Read_String();
                } else if (part == ModelVariables && field == 2) {
                    object.Transform = Read_Transform(reader);
                }
            });
        } else if (reader.Has_Children()) {
            Read_Physics_Parts(reader, object, depth + 1, saved_pointer);
        }
    });
}

struct SavedVehicle {
    HeadlessLevel::Vehicle State;
    uint32 PhysicsPointer = 0;
};

void Read_Vehicle_Parts(LevelReader &reader, SavedVehicle &vehicle, int depth = 0)
{
    reader.Require(depth < 24, "Vehicle inheritance chunks are nested too deeply");
    reader.Each_Chunk([&](uint32 part) {
        switch (part) {
        case GameObjectVariables:
        case DamageableVariables:
        case PhysicalObjectVariables:
        case DefenseVariables:
            reader.Each_Micro([&](uint32 field) {
                if (part == GameObjectVariables && field == 2) {
                    vehicle.State.DefinitionId = reader.Value<sint32>();
                } else if (part == DamageableVariables && field == 1) {
                    vehicle.State.PlayerType = reader.Value<sint32>();
                } else if (part == PhysicalObjectVariables && field == 10) {
                    vehicle.PhysicsPointer = reader.Value<uint32>();
                } else if (part == DefenseVariables) {
                    switch (field) {
                    case 3: vehicle.State.Health = reader.Value<float>(); break;
                    case 6: vehicle.State.Shield = reader.Value<float>(); break;
                    case 8: vehicle.State.ShieldSaveId = reader.Value<sint32>(); break;
                    }
                }
            });
            break;
        default:
            if (reader.Has_Children()) {
                Read_Vehicle_Parts(reader, vehicle, depth + 1);
            }
        }
    });
}

void Read_Vehicle_Physics(LevelReader &reader, std::map<uint32, HeadlessLevel::StaticObject> &physics)
{
    reader.In_Chunk(DynamicDataScene, [&] {
        reader.Each_Chunk([&](uint32 id) {
            if (id != DynamicObjectList) {
                return;
            }
            reader.Each_Chunk([&](uint32 record) {
                if (record != DynamicObjectRecord) {
                    return;
                }
                reader.Each_Chunk([&](uint32) {
                    // The Windows save links the game object to the PhysClass base pointer.
                    HeadlessLevel::StaticObject object;
                    uint32 saved_pointer = 0;
                    Read_Physics_Parts(reader, object, 0, &saved_pointer);
                    if (const auto wanted = physics.find(saved_pointer); wanted != physics.end()) {
                        wanted->second = std::move(object);
                    }
                });
            });
        });
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
                                Read_Physics_Parts(reader, object, 0);
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
    MapDefinitions map;
    Load_Map_Definitions(definitions, map);
    if (map_definitions) {
        Load_Map_Definitions(*map_definitions, map);
    }
    for (auto &object : LevelStaticObjects) {
        object.BuildingAggregate = object.FactoryId == PHYSICS_CHUNKID_BUILDINGAGGREGATE;
        if (object.BuildingAggregate) {
            const auto def = map.AggregateMCT.find(object.DefinitionId);
            object.MCT = def != map.AggregateMCT.end() && def->second;
        }
    }
    LevelReader reader(data);
    reader.In_Chunk(LevelData, [&] {
        std::vector<SavedVehicle> vehicles;
        reader.In_Chunk(CHUNKID_COMBAT, [&] {
            reader.Each_Chunk([&](uint32 part) {
                if (part == CombatSpawners) {
                    reader.Each_Chunk([&](uint32 id) {
                        if (id == SpawnerData) {
                            LevelSpawners.push_back(Read_Spawner(reader, map.Spawners));
                        }
                    });
                } else if (part == GameObjectManager) {
                    reader.In_Chunk(GameObjects, [&] {
                        reader.Each_Chunk([&](uint32 factory) {
                            if (factory == CHUNKID_GAME_OBJECT_VEHICLE) {
                                SavedVehicle vehicle;
                                reader.In_Chunk(FactoryData, [&] { Read_Vehicle_Parts(reader, vehicle); });
                                vehicles.push_back(std::move(vehicle));
                                return;
                            }
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
                            reader.Require(std::isfinite(building.Health) && std::isfinite(building.HealthMax) &&
                                building.Health >= 0 && building.Health <= building.HealthMax &&
                                std::isfinite(building.Shield) && building.Shield >= 0 &&
                                std::isfinite(building.DamagePoints) && std::isfinite(building.DeathPoints),
                                "Invalid building defense data");
                            if (building.CollectionCenter == Vector3(0, 0, 0)) {
                                building.CollectionCenter = building.Position;
                            }
                            if (const auto def = map.Buildings.find(building.DefinitionId); def != map.Buildings.end()) {
                                building.MeshPrefix = def->second.MeshPrefix;
                                building.MCTSkin = def->second.MCTSkin;
                                building.PlayerType = def->second.PlayerType.value_or(building.PlayerType);
                            }
                            LevelBuildings.push_back(std::move(building));
                        });
                    });
                }
            });
        });
        if (vehicles.empty()) {
            return;
        }
        std::map<uint32, StaticObject> physics;
        for (const auto &vehicle : vehicles) {
            if (vehicle.PhysicsPointer != 0) {
                physics.try_emplace(vehicle.PhysicsPointer);
            }
        }
        LevelReader physics_reader(data);
        physics_reader.In_Chunk(LevelData, [&] {
            physics_reader.In_Chunk(PHYSICS_CHUNKID_DYNAMIC_DATA_SUBSYSTEM, [&] {
                Read_Vehicle_Physics(physics_reader, physics);
            });
        });
        for (auto &saved : vehicles) {
            auto &vehicle = saved.State;
            const auto definition = map.Vehicles.find(vehicle.DefinitionId);
            if (definition == map.Vehicles.end() || definition->second.Kind == VehicleDefinition::Flying) {
                continue;
            }
            const auto part = physics.find(saved.PhysicsPointer);
            reader.Require(part != physics.end() && !part->second.AssetName.empty() && part->second.AssetName.size() < 256,
                "Vehicle is missing its saved physics model");
            reader.Require(std::isfinite(vehicle.Health) && vehicle.Health >= 0 &&
                std::isfinite(vehicle.Shield) && vehicle.Shield >= 0, "Invalid vehicle defense data");
            vehicle.Model = part->second.AssetName;
            vehicle.Transform = part->second.Transform;
            vehicle.Seats = definition->second.Seats;
            vehicle.UsesVehiclePhysics = definition->second.Kind != VehicleDefinition::Turret;
            if (vehicle.UsesVehiclePhysics) {
                vehicle.Entrances = definition->second.Entrances;
                vehicle.Exits = definition->second.Exits;
            }
            LevelVehicles.push_back(std::move(vehicle));
        }
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
