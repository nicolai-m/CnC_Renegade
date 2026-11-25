// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "aabox.h"
#include "coltype.h"
#include "connect.h"
#include "control.h"
#include "crc.h"
#include "combatchunkid.h"
#include "headless_collision.h"
#include "headless_join.h"
#include "headless_player_session.h"
#include "headless_simulation.h"
#include "netutil.h"
#include "networkobject.h"
#include "obbox.h"
#include "packetmgr.h"
#include "playertype.h"
#include "w3d_file.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void Check(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TestDirectory {
public:
    TestDirectory()
    {
        std::string pattern = (fs::temp_directory_path() / "renegade-fds-test-XXXXXX").string();
        Check(mkdtemp(pattern.data()) != nullptr, "Could not create test directory");
        Path = pattern;
    }

    ~TestDirectory()
    {
        std::error_code error;
        fs::remove_all(Path, error);
    }

    fs::path Path;
};

void Write_U32(std::ostream &out, uint32 number)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.put(static_cast<char>(number >> shift));
    }
}

std::string Chunk(uint32 id, const std::string &payload, bool children = true)
{
    std::ostringstream out;
    Write_U32(out, id);
    Write_U32(out, static_cast<uint32>(payload.size()) | (children ? 0x80000000u : 0));
    return out.str() + payload;
}

template<class T> std::string Bytes(const T &value)
{
    return std::string(reinterpret_cast<const char *>(&value), sizeof(value));
}

template<class T> std::string Micro(uint8 id, const T &value)
{
    return Bytes(id) + Bytes(static_cast<uint8>(sizeof(value))) + Bytes(value);
}

std::string Tree_Node(float x, float extent, uint32 children)
{
    const std::array<float, 6> box{x, 0, 0, extent, extent, extent};
    const auto variables = Bytes(uint8{0}) + Bytes(uint8{28}) + Bytes(box) + Bytes(children);
    return Chunk(0x101, Chunk(0x104, variables, false) + Chunk(0x103, "", false));
}

std::string Static_Data(uint32 last_link = 2, float world_extent = 20, bool with_buildings = false)
{
    const auto tree = Chunk(1, Bytes(uint32{0x10000}), false) + Tree_Node(0, world_extent, 3) +
        Tree_Node(6, 3, 0) + Tree_Node(-6, 3, 0);
    auto linked_object = [](uint32 node, uint32 id, const Vector3 &position, const auto &asset,
                             uint32 factory = 0x20002, sint32 definition = 0) {
        const std::array<float, 12> transform{1, 0, 0, position.X, 0, 1, 0, position.Y, 0, 0, 1, position.Z};
        const auto vars = Chunk(0x660055, Micro(3, uint32{15}) + Micro(6, definition) + Micro(7, id), false);
        const auto model = Chunk(0x660056, Chunk(CHUNKID_WW3D_BEGIN,
            Chunk(0x555040, Micro(1, asset) + Micro(2, transform), false)));
        return Chunk(0x770101, Chunk(factory, Chunk(0x00100101, vars + model))) +
            Chunk(0x770102, Chunk(0x200, Bytes(node), false));
    };
    auto objects = linked_object(1, 30, {6, 0, 0}, "TESTMESH") +
        linked_object(1, 31, {6, 0, 0}, "TESTMESH") + linked_object(last_link, 32, {-6, 0, 0}, "TESTMESH");
    if (with_buildings) {
        objects += linked_object(2, 33, {-4, 2, 1.62f}, "PLANT^WALL") +
            linked_object(2, 34, {-6, 0.8f, 1.62f}, "BASE_MCT", 0x20a03, 702);
    }
    return Chunk(CHUNKID_WWPHYS_BEGIN, Chunk(0x04433220, Chunk(0x4500, Chunk(0x104, tree)))) +
        Chunk(CHUNKID_WWPHYS_BEGIN + 1, Chunk(0x06090609, Chunk(0x770100, objects)));
}

std::string Collision_Data(uint32 last_vertex = 2)
{
    W3dMeshHeader3Struct mesh{};
    mesh.Attributes = W3D_MESH_FLAG_COLLISION_TYPE_PHYSICAL | W3D_MESH_FLAG_COLLISION_TYPE_PROJECTILE;
    std::strcpy(mesh.ContainerName, "testmesh");
    std::strcpy(mesh.MeshName, "terrain");
    mesh.NumVertices = 3;
    mesh.NumTris = 1;
    const W3dVectorStruct vertices[]{{-2, 0, 0}, {2, 0, 0}, {0, 3, 0}};
    const W3dTriStruct triangle{{0, 1, last_vertex}, SURFACE_TYPE_DIRT, {0, 0, 1}, 0};

    auto box = [](const char *name, uint32 attributes, W3dVectorStruct center, W3dVectorStruct extent) {
        W3dBoxStruct shape{};
        std::strcpy(shape.Name, name);
        shape.Attributes = attributes;
        shape.Center = center;
        shape.Extent = extent;
        return Chunk(W3D_CHUNK_BOX, Bytes(shape), false);
    };

    W3dHierarchyStruct hierarchy{};
    hierarchy.Version = W3D_MAKE_VERSION(4, 1);
    hierarchy.NumPivots = 2;
    std::strcpy(hierarchy.Name, "fixture_pivots");
    W3dPivotStruct pivots[2]{};
    pivots[0].ParentIdx = 0xffffffffu;
    pivots[1].Translation.Z = 1.5f;
    pivots[1].Rotation.Q[2] = std::sqrt(0.5f);
    pivots[1].Rotation.Q[3] = std::sqrt(0.5f);

    W3dHLodHeaderStruct hlod{};
    hlod.LodCount = 2;
    std::strcpy(hlod.Name, "testmesh");
    std::strcpy(hlod.HierarchyName, "fixture_pivots");
    W3dEmitterHeaderStruct emitter{};
    std::strcpy(emitter.Name, "testmesh.smoke");
    W3dHModelHeaderStruct hmodel{};
    hmodel.Version = W3D_MAKE_VERSION(4, 1);
    hmodel.NumConnections = 1;
    std::strcpy(hmodel.Name, "legacy");
    std::strcpy(hmodel.HierarchyName, "fixture_pivots");
    W3dHModelNodeStruct hmodel_node{};
    std::strcpy(hmodel_node.RenderObjName, "cover");
    auto part = [](const char *name, uint32 bone) {
        W3dHLodSubObjectStruct object{};
        object.BoneIndex = bone;
        std::strcpy(object.Name, name);
        return Chunk(W3D_CHUNK_HLOD_SUB_OBJECT, Bytes(object), false);
    };
    auto parts = [](uint32 id, uint32 count, const std::string &objects) {
        return Chunk(id, Chunk(W3D_CHUNK_HLOD_SUB_OBJECT_ARRAY_HEADER,
            Bytes(W3dHLodArrayHeaderStruct{count, 0}), false) + objects);
    };

    return Chunk(W3D_CHUNK_MESH, Chunk(W3D_CHUNK_MESH_HEADER3, Bytes(mesh), false) +
            Chunk(W3D_CHUNK_VERTICES, Bytes(vertices), false) +
            Chunk(W3D_CHUNK_TRIANGLES, Bytes(triangle), false)) +
        box("legacy.cover", W3D_BOX_ATTRIBUTE_ORIENTED | W3D_MESH_FLAG_COLLISION_TYPE_PROJECTILE |
            W3D_MESH_FLAG_COLLISION_TYPE_VEHICLE,
            {0, 0, 0}, {0.25f, 0.75f, 0.25f}) +
        box("testmesh.camera", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_CAMERA,
            {1, 0, 0}, {0.75f, 0.25f, 0.25f}) +
        box("testmesh.low", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_PHYSICAL,
            {0, 0, 0}, {2, 2, 1}) +
        box("testmesh.wall", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_PHYSICAL,
            {0, 2.8f, 1}, {0.5f, 0.05f, 1}) +
        box("testmesh.bulletwall", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_PROJECTILE,
            {-2.8f, 2.35f, 1.62f}, {0.08f, 0.17f, 0.8f}) +
        box("plant^wall", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_PROJECTILE,
            {0, 0, 0}, {0.3f, 0.3f, 0.3f}) +
        box("base_mct", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_PROJECTILE,
            {0, 0, 0}, {0.3f, 0.3f, 0.3f}) +
        Chunk(W3D_CHUNK_EMITTER, Chunk(W3D_CHUNK_EMITTER_HEADER, Bytes(emitter), false)) +
        Chunk(W3D_CHUNK_HIERARCHY, Chunk(W3D_CHUNK_HIERARCHY_HEADER, Bytes(hierarchy), false) +
            Chunk(W3D_CHUNK_PIVOTS, Bytes(pivots), false)) +
        Chunk(W3D_CHUNK_HMODEL, Chunk(W3D_CHUNK_HMODEL_HEADER, Bytes(hmodel), false) +
            Chunk(W3D_CHUNK_COLLISION_NODE, Bytes(hmodel_node), false)) +
        Chunk(W3D_CHUNK_HLOD, Chunk(W3D_CHUNK_HLOD_HEADER, Bytes(hlod), false) +
            parts(W3D_CHUNK_HLOD_LOD_ARRAY, 1, part("testmesh.low", 0)) +
            parts(W3D_CHUNK_HLOD_LOD_ARRAY, 2, part("testmesh.terrain", 0) + part("legacy", 1)) +
            parts(W3D_CHUNK_HLOD_AGGREGATE_ARRAY, 4, part("testmesh.camera", 1) + part("testmesh.smoke", 1) +
                part("testmesh.wall", 0) + part("testmesh.bulletwall", 0)));
}

std::string Dynamic_Data(float spawn_y = 2, bool with_buildings = false, float gdi_spawn_x = 6,
    bool with_vehicle = false)
{
    auto spawner = [spawn_y](sint32 id, sint32 definition, float x) {
        const std::array<float, 12> transform{1, 0, 0, x, 0, 1, 0, spawn_y, 0, 0, 1, 3};
        return Chunk(1014991133, Chunk(1014991054,
            Micro(1, id) + Micro(2, transform) + Micro(3, definition) + Micro(6, uint8{1}), false));
    };
    const auto spawners = Chunk(916991657, spawner(10, 100, -6) + spawner(20, 200, gdi_spawn_x));
    auto building = [](sint32 preset, sint32 id, std::array<float, 3> position, float health) {
        const std::array<float, 4> sphere{position[0], position[1], position[2], 5.0f};
        const auto defense = Chunk(914991020, Micro(3, health) + Micro(4, health) + Micro(5, uint32{17}) +
            Micro(6, 5.0f) + Micro(8, uint32{23}) + Micro(13, 1.0f) + Micro(14, 10.0f), false);
        return Chunk(CHUNKID_GAME_OBJECT_POWERPLANT, Chunk(0x00100101,
            Chunk(207011121, Micro(1, position) + Micro(2, uint8{0}) + Micro(3, sphere), false) +
            Chunk(207011120, Chunk(207011214, Micro(1, sint32{PLAYERTYPE_GDI}), false) +
                Chunk(910991407, Micro(2, preset) + Micro(3, id), false) + defense)));
    };
    const auto buildings = with_buildings ?
        building(300, 42, {-4, 2, 1.62f}, 30) + building(301, 43, {-6, 0.8f, 1.62f}, 80) :
        building(300, 42, {5, 1, 2}, 0);
    constexpr uint32 vehicle_pointer = 0x10203040;
    const auto vehicle = with_vehicle ? Chunk(CHUNKID_GAME_OBJECT_VEHICLE, Chunk(0x00100101,
        Chunk(910991407, Micro(2, sint32{310}), false) +
        Chunk(207011214, Micro(1, sint32{PLAYERTYPE_NEUTRAL}), false) +
        Chunk(910991146, Micro(10, vehicle_pointer), false) +
        Chunk(914991020, Micro(3, 125.0f) + Micro(6, 50.0f) + Micro(8, sint32{23}), false))) : "";
    const std::array<float, 12> transform{1, 0, 0, -6.5f, 0, 1, 0, 2, 0, 0, 1, 0.25f};
    const auto physical = Chunk(0x00100100, Bytes(vehicle_pointer + uint32{0x28}), false) + Chunk(0x00100101,
        Chunk(0x00660055, Micro(2, vehicle_pointer), false) +
        Chunk(0x00660056, Chunk(0x10000, Chunk(0x00555040, Micro(1, "TEST_CAR") + Micro(2, transform), false))));
    const auto dynamic = with_vehicle ? Chunk(CHUNKID_WWPHYS_BEGIN + 0x50, Chunk(0x7001,
        Chunk(0x890100, Chunk(0x890101, Chunk(0x2010a, physical))))) : "";
    const auto objects = Chunk(916991654, Chunk(916991653, buildings + vehicle));
    return Chunk(1011991649, Chunk(CHUNKID_COMBAT, objects + spawners) + dynamic);
}

void Check_Box_Sweeps(const HeadlessCollision &collision)
{
    const Vector3 drop(0, 0, -4);
    const Vector3 soldier_extent(0.3f, 0.3f, 0.5f);
    const auto soldier = collision.Sweep_Box(AABoxClass(Vector3(6.5f, 0.125f, 3), soldier_extent),
        drop, COLLISION_TYPE_PHYSICAL);
    Check(soldier && soldier->ObjectIndex == 0 && !soldier->StartsInside &&
        std::abs(soldier->Fraction - 0.625f) < 0.0001f && soldier->Normal == Vector3(0, 0, 1) &&
        soldier->ContactPoint && std::abs(soldier->ContactPoint->Z) < 0.0001f &&
        soldier->Surface == SURFACE_TYPE_DIRT, "Soldier sweep did not return the terrain contact");

    const AABoxClass edge(Vector3(9.125f, 0.5f, 3), Vector3(1.5f, 0.25f, 0.5f));
    Check(!collision.Cast_Ray(edge.Center, edge.Center + drop, COLLISION_TYPE_PHYSICAL) &&
        collision.Sweep_Box(edge, drop, COLLISION_TYPE_PHYSICAL),
        "The full box must reach terrain even when its center ray lies outside the spatial node");
    Check(!collision.Sweep_Box(AABoxClass(Vector3(7.9f, 2, 3), Vector3(0.1f, 0.1f, 0.5f)),
        drop, COLLISION_TYPE_PHYSICAL), "Triangle bounds alone must not block a sweep");

    const AABoxClass touching(Vector3(6, 1, 0.5f), soldier_extent);
    Check(!collision.Sweep_Box(touching, Vector3(0.25f, 0, 0), COLLISION_TYPE_PHYSICAL) &&
        !collision.Sweep_Box(touching, -drop, COLLISION_TYPE_PHYSICAL), "Contact must allow sliding and moving away");
    const auto overlap = collision.Sweep_Box(AABoxClass(Vector3(6, 1, 0.1f), soldier_extent),
        Vector3(0, 0, 0), COLLISION_TYPE_PHYSICAL);
    Check(overlap && overlap->StartsInside && overlap->Fraction == 0 && !overlap->ContactPoint &&
        overlap->Surface == SURFACE_TYPE_DIRT, "Stationary ground penetration was not detected");

    constexpr unsigned vehicle_mask = COLLISION_TYPE_PHYSICAL | COLLISION_TYPE_VEHICLE;
    const auto yaw = Create_Z_Rotation_Matrix3(0.7853981634f);
    const OBBoxClass vehicle(Vector3(6.5f, 2, 1.5f), Vector3(0.5f, 0.2f, 0.25f), yaw);
    const Vector3 drive(0, -4, 0);
    const float expected = (2.0f - 0.25f - vehicle.Project_To_Axis(Vector3(0, 1, 0))) / 4;
    const auto cover = collision.Sweep_Box(vehicle, drive, vehicle_mask);
    Check(cover && !cover->StartsInside && std::abs(cover->Fraction - expected) < 0.0001f &&
        (cover->Normal - Vector3(0, 1, 0)).Length() < 0.0001f && cover->ContactPoint &&
        std::abs(cover->ContactPoint->Y - 0.25f) < 0.0001f,
        "Rotated vehicle sweep did not return a world-space contact on the vehicle collider");
    Check(!collision.Sweep_Box(vehicle, drive, COLLISION_TYPE_PHYSICAL),
        "The physical-only mask must ignore the vehicle collider");

    const OBBoxClass vehicle_drop(Vector3(6.5f, 0.125f, 3), vehicle.Extent, yaw);
    const auto first = collision.Sweep_Box(vehicle_drop, drop, vehicle_mask);
    Check(first && !first->StartsInside && std::abs(first->Fraction - 0.25f) < 0.0001f &&
        first->ContactPoint && std::abs(first->ContactPoint->Z - 1.75f) < 0.0001f,
        "The nearer vehicle collider must stop the sweep before the terrain");

    const OBBoxClass aligned_target(Vector3(7.5f, 0.125f, 3), vehicle.Extent, yaw);
    const auto camera = collision.Sweep_Box(aligned_target, drop, COLLISION_TYPE_CAMERA);
    Check(camera && !camera->StartsInside && camera->ContactPoint &&
        std::abs(camera->ContactPoint->Z - 1.75f) < 0.0001f,
        "The sweep lost the aligned collider offset within its rotated hierarchy");
}

void Check_Soldier_Simulation(const HeadlessCollision &collision)
{
    HeadlessSimulation simulation(collision);
    Check(!simulation.Spawn_Soldier(0, PLAYERTYPE_GDI) && !simulation.Spawn_Soldier(3, PLAYERTYPE_SPECTATOR),
        "Only valid players on a combat team can spawn");
    Check(simulation.Spawn_Soldier(1, PLAYERTYPE_GDI) && simulation.Spawn_Soldier(2, PLAYERTYPE_NOD) &&
        !simulation.Spawn_Soldier(1, PLAYERTYPE_NOD), "A team spawn should create one soldier per player");
    const auto &gdi = *simulation.Find_Soldier(1);
    const auto &nod = *simulation.Find_Soldier(2);
    Check(gdi.SpawnerId == 20 && gdi.Position.X == 6 && gdi.Grounded &&
        gdi.Position.Z >= 0 && gdi.Position.Z < 0.01f &&
        nod.SpawnerId == 10 && nod.Position.X == -6 && nod.Grounded,
        "Soldiers must start on the ground at their team's saved spawn");

    Check(simulation.Set_Move_Input(1, Vector2(0, 1)) &&
        !simulation.Set_Move_Input(1, Vector2(0, std::numeric_limits<float>::quiet_NaN())) &&
        !simulation.Set_Move_Input(3, Vector2(0, 1)), "Only valid input for a spawned soldier should be accepted");
    const auto spawn_position = gdi.Position;
    simulation.Advance(19ms);
    Check(gdi.Position == spawn_position, "An incomplete tick changed the soldier position");
    simulation.Advance(1ms);
    Check(std::abs(gdi.Position.Y - (spawn_position.Y + 0.08f)) < 0.0001f,
        "A fixed tick did not move the soldier at the configured speed");
    for (int tick = 0; tick < 12; ++tick) {
        simulation.Advance(20ms);
    }
    Check(std::abs(gdi.Position.Y - 2.448f) < 0.002f && gdi.Grounded,
        "The moving soldier penetrated or stopped short of the wall");

    Check(simulation.Set_Move_Input(1, Vector2(1, 1)), "Diagonal input was rejected");
    simulation.Advance(40ms);
    Check(std::abs(gdi.Position.X - (6.0f + std::sqrt(0.5f) * 0.16f)) < 0.001f &&
        std::abs(gdi.Position.Y - 2.448f) < 0.002f && gdi.Grounded,
        "The soldier should slide along the wall at a normalized diagonal speed");

    Check(simulation.Set_Move_Input(2, Vector2(0, -1)), "Nod movement input was rejected");
    simulation.Advance(5s);
    Check(std::abs(nod.Position.Y - 1.6f) < 0.0001f,
        "After a long stall the simulation should catch up by at most five ticks");
    for (int tick = 0; tick < 30; ++tick) {
        simulation.Advance(20ms);
    }
    Check(nod.Position.Y < -0.5f && nod.Position.Z < -0.05f && !nod.Grounded,
        "A soldier who walks off the terrain should fall");
}

std::string Definitions(bool with_repair_weapon = false)
{
    auto definition = [](sint32 id, sint32 team) {
        return Chunk(CHUNKID_SPAWNER_DEF, Chunk(0x00100101,
            Chunk(1013991542, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1013991543, Micro(3, team) + Micro(9, uint8{0}) + Micro(15, uint8{1}), false)));
    };
    auto soldier = [with_repair_weapon](sint32 id, const auto &name, sint32 physics, sint32 charge = 620) {
        const auto identity = Chunk(0x100, Micro(1, id) + Micro(3, name), false);
        const auto defense = Chunk(7311607, Micro(0, 100.0f) + Micro(2, sint32{17}) +
            Micro(3, 50.0f) + Micro(5, sint32{23}) + Micro(6, 0.5f) + Micro(7, 4.0f), false);
        const auto physical = Chunk(418001829, Chunk(909991657, Micro(18, physics), false));
        const auto weapons = Chunk(418001830, Micro(11, sint32{600 + id % 2}) + Micro(12, sint32{500}) +
            Micro(14, charge), false);
        auto grants = Micro(2, "M00_GrantPowerup_Created") + Micro(3, "POW_Pistol");
        if (charge == 0) {
            grants += Micro(2, "M00_GrantPowerup_Created") + Micro(3, "POW_Proximity");
        }
        if (with_repair_weapon && (charge == 624 || charge == 0)) {
            grants += Micro(2, "M00_GrantPowerup_Created") + Micro(3, "POW_Repair");
        }
        const auto scripts = Chunk(627001057, grants, false);
        return Chunk(CHUNKID_GAME_OBJECT_DEF_SOLDIER,
            Chunk(0x00100101, identity + defense + physical + weapons + scripts));
    };
    auto physics = [](sint32 id, const char (&model)[18]) {
        return Chunk(0x20501, Chunk(0x00100101, Chunk(0x100, Micro(1, id), false) +
            Chunk(0x055ffe08, Micro(1, model), false)));
    };
    auto weapon = [](sint32 id, sint32 clip, sint32 reserve, float key, sint32 style,
                      sint32 primary = 640, sint32 secondary = 641) {
        return Chunk(CHUNKID_WEAPON_DEF, Chunk(0x00100101,
            Chunk(1205091655, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1205091654, Micro(1, style) + Micro(9, 0.35f) + Micro(27, primary) +
                Micro(28, secondary) + Micro(29, clip) +
                Micro(35, reserve) + Micro(37, key), false)));
    };
    auto ammo = [](sint32 id, sint32 cost, float speed) {
        return Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
            Chunk(1206091430, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1206091429, Micro(3, sint32{1}) + Micro(4, 30.0f) + Micro(5, 40.0f) +
                Micro(6, speed) + Micro(31, cost) + Micro(48, 2.0f), false)));
    };
    const auto powerup = Chunk(CHUNKID_GAME_OBJECT_DEF_POWERUP, Chunk(0x00100101,
        Chunk(909991656, Chunk(0x100, Micro(1, sint32{630}) + Micro(3, "pow_pistol"), false)) +
        Chunk(909991657, Micro(8, sint32{610}) + Micro(9, uint8{1}) + Micro(10, sint32{-1}), false)));
    const auto mine_powerup = Chunk(CHUNKID_GAME_OBJECT_DEF_POWERUP, Chunk(0x00100101,
        Chunk(909991656, Chunk(0x100, Micro(1, sint32{631}) + Micro(3, "pow_proximity"), false)) +
        Chunk(909991657, Micro(8, sint32{625}) + Micro(9, uint8{1}) + Micro(10, sint32{2}), false)));
    const auto repair_powerup = Chunk(CHUNKID_GAME_OBJECT_DEF_POWERUP, Chunk(0x00100101,
        Chunk(909991656, Chunk(0x100, Micro(1, sint32{632}) + Micro(3, "pow_repair"), false)) +
        Chunk(909991657, Micro(8, sint32{626}) + Micro(9, uint8{1}) + Micro(10, sint32{-1}), false)));
    auto building = [](sint32 id, const auto &prefix) {
        return Chunk(CHUNKID_GAME_OBJECT_DEF_POWERPLANT, Chunk(0x00100101,
            Chunk(0x100, Micro(1, id), false) + Chunk(207011031, Micro(1, prefix) + Micro(2, uint32{1}), false) +
            Chunk(207011206, Micro(6, sint32{PLAYERTYPE_GDI}), false)));
    };
    const auto terminal = Chunk(0x20c03, Chunk(0x00100101,
        Chunk(0x100, Micro(1, sint32{702}), false) + Chunk(8281442, Micro(4, uint8{1}), false)));
    const auto thrown_charge = Chunk(CHUNKID_GAME_OBJECT_DEF_C4, Chunk(0x00100101,
        Chunk(930991700, Chunk(0x100, Micro(1, sint32{621}) + Micro(3, "Tossed C4"), false) +
            Chunk(909991657, Micro(18, sint32{502}), false) +
            Chunk(7311607, Micro(0, 100.0f) + Micro(2, sint32{25}) +
                Micro(5, sint32{with_repair_weapon ? 25 : 23}) + Micro(7, 30.0f), false)) +
        Chunk(930991701, Micro(1, 10.0f), false)));
    const auto charge_physics = Chunk(0x20506, Chunk(0x00100101,
        Chunk(0x100, Micro(1, sint32{502}), false) + Chunk(0x04486001, Micro(1, 1.5f), false)));
    const auto charge_ammo = Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
        Chunk(1206091430, Chunk(0x100, Micro(1, sint32{642}), false)) +
        Chunk(1206091429, Micro(2, "test\\charge.w3d") + Micro(24, sint32{643}) + Micro(31, sint32{1}) +
            Micro(32, sint32{2}) + Micro(35, 3.0f) + Micro(36, 1.5f) + Micro(37, 6.0f) + Micro(48, 10.0f), false)));
    const auto remote_ammo = Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
        Chunk(1206091430, Chunk(0x100, Micro(1, sint32{646}), false)) +
        Chunk(1206091429, Micro(2, "test\\charge.w3d") + Micro(24, sint32{643}) + Micro(31, sint32{1}) +
            Micro(32, sint32{1}) + Micro(48, 10.0f), false)));
    const auto proximity_ammo = Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
        Chunk(1206091430, Chunk(0x100, Micro(1, sint32{647}), false)) +
        Chunk(1206091429, Micro(2, "test\\charge.w3d") + Micro(24, sint32{643}) + Micro(31, sint32{1}) +
            Micro(32, sint32{3}) + Micro(35, 0.0f) + Micro(36, 0.8f) + Micro(37, 0.0f) +
            Micro(38, 0.25f) + Micro(39, 1.9f) + Micro(40, 0.5f) + Micro(48, 10.0f), false)));
    const auto charge_explosion = Chunk(CHUNKID_EXPLOSION_DEF, Chunk(0x00100101,
        Chunk(0317001526, Chunk(0x100, Micro(1, sint32{643}), false)) +
        Chunk(0317001525, Micro(3, 3.0f) + Micro(4, 45.0f) + Micro(5, uint32{0}) + Micro(6, uint8{0}), false)));
    const auto repair_ammo = Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
        Chunk(1206091430, Chunk(0x100, Micro(1, sint32{648}), false)) +
        Chunk(1206091429, Micro(3, sint32{1}) + Micro(4, -20.0f) + Micro(5, 4.0f) + Micro(6, 400.0f) +
            Micro(48, 2.0f) + Micro(49, uint8{1}), false)));
    const auto disarm_ammo = Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
        Chunk(1206091430, Chunk(0x100, Micro(1, sint32{650}) + Micro(3, "KilledC4"), false)) +
        Chunk(1206091429, Micro(24, sint32{649}), false)));
    const auto disarm_effect = Chunk(CHUNKID_EXPLOSION_DEF, Chunk(0x00100101,
        Chunk(0317001526, Chunk(0x100, Micro(1, sint32{649}), false)) +
        Chunk(0317001525, Micro(3, 0.0f) + Micro(4, 0.0f), false)));
    const std::array<float, 15> entry_zone{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1.4f, 1.1f, 1.0f};
    const std::array<float, 12> exit_transform{1, 0, 0, 0.5f, 0, 1, 0, -0.65f, 0, 0, 1, 0};
    const auto vehicle = Chunk(CHUNKID_GAME_OBJECT_DEF_VEHICLE, Chunk(0x00100101,
        Chunk(0x100, Micro(1, sint32{310}), false) +
        Chunk(930991657, Micro(1, sint32{0}) + Micro(32, sint32{2}), false) +
        Chunk(0x11051106, Micro(1, sint32{8}) + Micro(2, entry_zone), false) +
        Chunk(0x11051106, Micro(1, sint32{9}) + Micro(4, exit_transform), false)));
    return Chunk(CHUNKID_SAVELOAD_DEFMGR, Chunk(0x101,
        definition(100, PLAYERTYPE_NOD) + definition(200, PLAYERTYPE_GDI) + vehicle +
        building(300, "plant") + building(301, "base") + terminal +
        soldier(400, "CnC_Nod_Minigunner_0", 500) + soldier(401, "CnC_GDI_MiniGunner_0", 501) +
        soldier(402, "CnC_Nod_Engineer_0", 500, 624) + soldier(403, "CnC_GDI_Engineer_0", 501, 624) +
        soldier(404, "CnC_Nod_Technician_0", 500, 0) + soldier(405, "CnC_GDI_Engineer_2SF", 501, 0) +
        physics(500, "test\\soldier0.w3d") + physics(501, "test\\soldier1.w3d") +
        weapon(600, 40, 200, 2, 2) + weapon(601, 40, 200, 2, 2) + weapon(610, 12, 100, 1, 5) +
        weapon(620, 1, 0, 7, 0, 642, 642) + ammo(640, 20, 40.0f) + ammo(641, 7, 400.0f) + powerup +
        weapon(624, 1, 1, 7, 0, 646, 646) + weapon(625, 1, 1, 7, 0, 647, 647) + mine_powerup +
        thrown_charge + charge_physics + charge_ammo + remote_ammo + proximity_ammo + charge_explosion +
        weapon(626, -1, -1, 4, 3, 648, 648) + repair_powerup + repair_ammo + disarm_ammo + disarm_effect));
}

struct ArchiveEntry {
    const char *Name;
    std::string Data;
};

void Write_Archive(const fs::path &path, const std::vector<ArchiveEntry> &entries)
{
    struct IndexEntry { uint32 Crc, Offset, Length; };
    std::vector<IndexEntry> index;
    std::ofstream out(path, std::ios::binary);
    out.write("MIX1", 4);
    Write_U32(out, 0);
    Write_U32(out, 0);
    Write_U32(out, 0);
    for (const auto &entry : entries) {
        index.push_back({CRC::String(entry.Name), static_cast<uint32>(out.tellp()),
            static_cast<uint32>(entry.Data.size())});
        out.write(entry.Data.data(), entry.Data.size());
    }
    auto index_offset = static_cast<uint32>(out.tellp());
    std::sort(index.begin(), index.end(), [](const IndexEntry &a, const IndexEntry &b) { return a.Crc < b.Crc; });
    Write_U32(out, index.size());
    for (const auto &entry : index) {
        Write_U32(out, entry.Crc);
        Write_U32(out, entry.Offset);
        Write_U32(out, entry.Length);
    }
    out.seekp(4);
    Write_U32(out, index_offset);
    Check(bool(out), "Could not create fixture archive");
}

int process_count = 0;

class FdsProcess {
public:
    FdsProcess(const char *binary, const fs::path &root, const fs::path &config,
        const char *option = nullptr, const char *value = nullptr)
        : Log(root / ("fds-" + std::to_string(++process_count) + ".log"))
    {
        Pid = fork();
        Check(Pid >= 0, "Could not fork FDS process");
        if (Pid == 0) {
            const int output = open(Log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) {
                _exit(127);
            }
            if (output > STDERR_FILENO) {
                close(output);
            }
            execl(binary, binary, "--data-dir", root.c_str(), "--config", config.c_str(), option, value,
                static_cast<char *>(nullptr));
            _exit(127);
        }
    }

    ~FdsProcess()
    {
        if (Pid > 0) {
            kill(Pid, SIGKILL);
            while (waitpid(Pid, nullptr, 0) == -1 && errno == EINTR) {}
        }
    }

    std::string Output() const
    {
        std::ifstream input(Log);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    int Wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            int status = 0;
            const auto result = waitpid(Pid, &status, WNOHANG);
            if (result == Pid) {
                Pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            Check(result >= 0 || errno == EINTR, "Could not wait for FDS process");
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("FDS process did not exit: " + Output());
    }

    unsigned short Wait_For_Port() const
    {
        const std::string ready = "FDS bootstrap listening on 127.0.0.1:";
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            const auto output = Output();
            const auto marker = output.find(ready);
            if (marker != std::string::npos && output.find('\n', marker) != std::string::npos) {
                const int port = std::stoi(output.substr(marker + ready.size()));
                Check(port > 0 && port <= MAX_SERVER_PORT, "FDS reported an invalid port");
                return static_cast<unsigned short>(port);
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("FDS did not start listening: " + Output());
    }

    void Stop()
    {
        Check(kill(Pid, SIGTERM) == 0, "Could not signal FDS process");
        const auto status = Wait();
        Check(status == 0, "FDS failed graceful shutdown: " + Output());
    }

private:
    fs::path Log;
    pid_t Pid = -1;
};

REFUSAL_CODE refusal = REFUSAL_CLIENT_ACCEPTED;

void Refused(REFUSAL_CODE code) { refusal = code; }

void Check_Join_Refused(unsigned short port)
{
    Check(cNetUtil::Protocol_Init(false), "IPv4 unavailable");
    cConnection client;
    client.Init_As_Client(htonl(INADDR_LOOPBACK), port);
    client.Install_Refusal_Handler(Refused);
    client.Set_Bandwidth_Budget_Out(128000);
    cPacket request;
    request.Add(128000);
    client.Connect_Cs(request);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        client.Service_Read();
        client.Service_Send(true);
        std::this_thread::sleep_for(5ms);
    } while (!client.Is_Destroy() && std::chrono::steady_clock::now() < deadline);
    Check(refusal == REFUSAL_BY_APPLICATION && !client.Is_Established(),
        "FDS bootstrap should refuse a join until the game session is ready");
}

constexpr UINT MovementTestToken = 0x52454e47u;
constexpr int SoldierObject = 1500000000;
constexpr int FirstControlObject = 2110000001;
constexpr int TestBandwidth = 1000000;

struct MovementSnapshot {
    Vector3 Position;
    Vector3 Velocity{0, 0, 0};
    bool Airborne = false;
};

std::vector<MovementSnapshot> MovementSnapshots;

void Receive_Movement_Snapshot(cPacket &packet)
{
    int object;
    BYTE dirty;
    bool flag;
    Check(packet.Get(object) == SoldierObject && packet.Get(dirty) == 1 &&
        !packet.Get(flag) && !packet.Get(flag) && !packet.Get(flag),
        "Server did not publish an original-format soldier update");
    MovementSnapshot snapshot;
    ULONG value;
    for (int axis = 0; axis < 3; ++axis) {
        packet.Get_Bits(value, 8);
        snapshot.Position[axis] = -21.0f + static_cast<float>(value) * (42.0f / 255.0f);
    }
    packet.Get_Bits(value, 5);
    Check(value == 0 || value == 5, "The soldier should publish the upright or airborne state");
    snapshot.Airborne = value == 5;
    packet.Get_Bits(value, 9);
    if (snapshot.Airborne) {
        packet.Get(snapshot.Velocity.X);
        packet.Get(snapshot.Velocity.Y);
        packet.Get(snapshot.Velocity.Z);
    }
    Check(!packet.Get(flag) && !packet.Get(flag), "Unexpected damage or host bone");
    packet.Get_Bits(value, 24); // Aiming target.
    packet.Get_Bits(value, 4); // Continuous control flags.
    packet.Get_Bits(value, 32); // Four analog axes.
    Check(packet.Is_Flushed(), "Soldier update had an unexpected field layout");
    MovementSnapshots.push_back(snapshot);
}

void Write_Movement(cPacket &packet, int control_object, int owner, int soldier, const Vector3 &target,
    float position_extent = 21.0f, unsigned position_bits = 8, ULONG buttons = 0,
    ULONG continuous = 0, bool moving = true)
{
    packet.Add(control_object);
    packet.Add(static_cast<BYTE>(owner ? 15 : 1));
    packet.Add(false);
    if (owner) {
        packet.Add(1018); // NETCLASSID_CLIENTCONTROL.
        packet.Add(owner);
    }
    packet.Add(soldier);
    if (soldier == -1) {
        return;
    }
    packet.Add_Bits(buttons, ControlClass::NUM_BOOLEAN_ONE_TIME);
    packet.Add_Bits(continuous, ControlClass::NUM_BOOLEAN_CONTINUOUS);
    packet.Add_Bits(moving ? 255 : 128, 8); // Forward input.
    packet.Add_Bits(128, 8); // Neutral strafe.
    packet.Add_Bits(128, 8); // Neutral vertical input.
    packet.Add_Bits(128, 8); // Neutral turning input.
    packet.Add(false); // Sniping.
    packet.Add(true); // Optional checksum follows.
    packet.Add(12345);
    const float max_code = static_cast<float>((1u << position_bits) - 1);
    for (const float coordinate : {target.X, target.Y, target.Z}) {
        const auto code = static_cast<ULONG>(std::round((coordinate + position_extent) * max_code /
            (position_extent * 2)));
        packet.Add_Bits(code, position_bits);
    }
}

void Check_Player_Transport(const HeadlessCollision &collision)
{
    Check(cNetUtil::Protocol_Init(false), "IPv4 unavailable for player updates");
    PacketManager.Set_Is_Server(true);
    cConnection server;
    Check(server.Init_As_Server(0, 2, true, INADDR_LOOPBACK), "Could not bind movement test server");
    server.Set_Bandwidth_Budget_Out(TestBandwidth);
    HeadlessPlayerSession session(collision, server);
    server.Install_Application_Acceptance_Handler([](cPacket &packet) {
        UINT token;
        return packet.Get_Bits_Remaining() == 64 && packet.Get(token) == MovementTestToken ?
            REFUSAL_CLIENT_ACCEPTED : REFUSAL_BY_APPLICATION;
    });
    server.Install_Conn_Handler([&](int id) {
        Check(session.Attach_Player(id, PLAYERTYPE_GDI, SoldierObject, {}), "Could not attach the test soldier");
        server.Set_Rhost_Is_In_Game(id, true);
    });
    int received = 0;
    server.Install_Server_Packet_Handler([&](cPacket &packet, int id) {
        session.Receive_Control(packet, id);
        ++received;
    });
    server.Install_Server_Broken_Connection_Handler([&](int id) { session.Remove_Player(id); });
    server.Install_Eviction_Handler([&](int id) { session.Remove_Player(id); });

    cConnection client;
    client.Init_As_Client(htonl(INADDR_LOOPBACK), server.Get_Local_Port());
    client.Set_Bandwidth_Budget_Out(TestBandwidth);
    client.Install_Accept_Handler([] {});
    client.Install_Client_Packet_Handler(Receive_Movement_Snapshot);
    cPacket request;
    request.Add(MovementTestToken);
    request.Add(TestBandwidth);
    client.Connect_Cs(request);

    auto service_until = [&](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            client.Service_Send(true);
            server.Service_Read();
            server.Service_Send(true);
            client.Service_Read();
            if (ready()) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("Timed out exchanging movement test packets");
    };
    service_until([&] { return client.Is_Established(); });
    Check(client.Get_Local_Id() == 1, "Movement test has an unexpected peer ID");

    const auto send = [&](cPacket &packet, BYTE mode) {
        const auto before = received;
        client.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, mode);
        service_until([&] { return received > before; });
    };
    const auto snapshot = [&](auto elapsed) {
        const auto before = MovementSnapshots.size();
        session.Advance(elapsed);
        service_until([&] { return MovementSnapshots.size() > before; });
        return MovementSnapshots.back();
    };
    const auto spawn = snapshot(50ms).Position;

    cPacket wrong_owner;
    Write_Movement(wrong_owner, FirstControlObject, 2, SoldierObject, {0, 8, 0});
    send(wrong_owner, SEND_RELIABLE);
    cPacket unregistered;
    Write_Movement(unregistered, FirstControlObject, 0, SoldierObject, {0, 8, 0});
    send(unregistered, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position == spawn, "An invalid control owner moved the soldier");

    cPacket creation;
    Write_Movement(creation, FirstControlObject, 1, -1, {0, 0, 0});
    send(creation, SEND_RELIABLE);
    cPacket wrong_soldier;
    Write_Movement(wrong_soldier, FirstControlObject, 0, SoldierObject + 1, {0, 8, 0});
    send(wrong_soldier, SEND_UNRELIABLE);
    cPacket truncated;
    Write_Movement(truncated, FirstControlObject, 0, SoldierObject, {0, 8, 0});
    truncated.Set_Bit_Length(truncated.Get_Bit_Length() - 1);
    send(truncated, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position == spawn, "Wrong-target or truncated input moved the soldier");

    cPacket drive;
    Write_Movement(drive, FirstControlObject, 0, SoldierObject, {0, 8, 0});
    send(drive, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position.Y > spawn.Y + 0.2f, "Valid client movement did not reach the simulation");
    const auto wall = snapshot(100ms).Position;
    Check(std::abs(wall.Y - 2.448f) < 0.1f, "Networked soldier did not stop at the wall");

    cPacket retreat;
    Write_Movement(retreat, FirstControlObject, 0, SoldierObject, {0, -8, 0});
    send(retreat, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position.Y < wall.Y - 0.2f, "Turning the aim did not change the movement direction");
    const auto before_timeout = snapshot(120ms).Position;
    Check(snapshot(100ms).Position == before_timeout, "Expired movement input continued driving the soldier");
    session.Remove_Player(client.Get_Local_Id());
    Check(session.Attach_Player(1, PLAYERTYPE_GDI, SoldierObject, {}) && snapshot(50ms).Position == spawn,
        "Reattaching a peer retained the previous soldier's position or input");

    cPacket recreate;
    Write_Movement(recreate, FirstControlObject + 1, 1, SoldierObject, {0, -8, 0});
    send(recreate, SEND_RELIABLE);
    snapshot(100ms);
    MovementSnapshot edge;
    for (int i = 0; i < 7; ++i) {
        cPacket walk;
        Write_Movement(walk, FirstControlObject + 1, 0, SoldierObject, {0, -8, 0});
        send(walk, SEND_UNRELIABLE);
        edge = snapshot(100ms);
    }
    Check(edge.Airborne && edge.Velocity.Z < 0.0f && edge.Position.Y < -0.3f,
        "Falling off the floor did not publish airborne state and downward velocity");
}

template<class T> T Field(cPacket &packet)
{
    T value;
    return packet.Get(value);
}

struct Join_Client_State {
    struct Defense {
        bool Dead;
        ULONG Health;
        ULONG Shield;
    };

    std::set<int> Teams;
    std::map<int, int> TeamObjects; // Network object ID to team.
    std::map<int, std::pair<int, int>> TeamCounts;
    std::map<int, float> TeamScores;
    std::map<int, int> Players; // Peer ID to network object ID.
    std::map<int, std::pair<int, int>> PlayerCounts; // By network object ID.
    std::map<int, float> PlayerScores;
    std::map<int, float> PlayerCredits;
    std::vector<std::pair<int, int>> KillMessages;
    std::map<int, int> Soldiers;
    std::map<int, float> Y;
    std::map<int, int> SelectedWeapon;
    std::map<int, int> Rounds;
    std::map<int, ULONG> FireControls;
    std::map<int, Defense> Vitals;
    std::map<int, int> Buildings; // Preset ID to network object ID.
    std::map<int, Defense> BuildingVitals;
    std::map<int, bool> BuildingDestroyed;
    std::map<int, int> VehicleDriver;
    std::set<int> SeatedSoldiers;
    struct Charge {
        int Owner = 0;
        bool Stuck = false;
        bool MCT = false;
        int AttachedObject = 0;
        ULONG Health = 0;
    };
    std::map<int, Charge> Charges;
    std::vector<int> ExplosionOwners;
    std::vector<int> DisarmOwners;
    std::set<int> Deleted;
    std::optional<float> OptionsTime;
    std::optional<int> GameDataTime;
    int Round = 1;
    uint32 MapCRC = 0;
    bool CycleEnded = false;
    bool JoiningActiveCombat = false;
    bool BaseDestructionEndsGame = false;
    bool CanRepairBuildings = false;
    bool RepairFixture = false;
    ULONG ShieldArmorCode = 2;
    int LastWinner = -1;
    int LastWinReason = 0;
    uint32 LastWinDuration = 0;
};

Join_Client_State FirstJoinClient;
Join_Client_State SecondJoinClient;
Join_Client_State LateJoinClient;
Join_Client_State LoadingJoinClient;
unsigned short JoinPort;
uint32 JoinKey;
const uint32 TestMapCRC = CRC::String("C&C_TEST.MIX");
const uint32 NextMapCRC = CRC::String("C&C_NEXT.MIX");

int Check_Game_Info(cPacket &packet, uint32 map_crc = TestMapCRC)
{
    Check(Field<uint32>(packet) == htonl(INADDR_LOOPBACK), "Wrong advertised server address");
    WCHAR owner[64];
    WCHAR title[64];
    packet.Get_Wide_Terminated_String(owner, 64);
    packet.Get_Wide_Terminated_String(title, 64);
    Check(std::u16string(owner) == u"FixtureHost" && std::u16string(title) == u"Join fixture" &&
        Field<int>(packet) == JoinPort, "Game info did not describe this server");
    const int players = Field<int>(packet);
    Check(Field<int>(packet) == 16 && Field<uint32>(packet) == JoinKey &&
        Field<uint32>(packet) != 0 && Field<uint32>(packet) != 0, "Wrong version or capacity in game info");
    Check(Field<bool>(packet) && !Field<bool>(packet) && Field<bool>(packet) &&
        !Field<bool>(packet) && !Field<bool>(packet), "Incorrect game info flags");
    Check(Field<uint32>(packet) == map_crc && Field<uint32>(packet) == 0, "Wrong map in game info");
    return players;
}

class LanReceiver {
public:
    LanReceiver()
    {
        cNetUtil::Create_Unbound_Socket(Socket);
        sockaddr_in address{};
        cNetUtil::Create_Local_Address(&address, 3373);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (Socket == INVALID_SOCKET || bind(Socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
            cNetUtil::Close_Socket(Socket);
            throw std::runtime_error("Could not listen on the original LAN discovery port");
        }
    }

    LanReceiver(const LanReceiver &) = delete;
    LanReceiver &operator=(const LanReceiver &) = delete;

    ~LanReceiver() { cNetUtil::Close_Socket(Socket); }

    void Expect(bool hosting, int players = 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            cPacket packet;
            const auto size = recv(Socket, packet.Get_Data(), packet.Get_Max_Size(), MSG_TRUNC);
            if (size < 0 && (errno == EAGAIN || errno == EINTR)) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            Check(size > 0 && size <= packet.Get_Max_Size(), "Invalid LAN announcement length");
            packet.Set_Bit_Length(size * 8);
            Check(Field<BYTE>(packet) == 0, "LAN announcement was not a raw position message");
            WCHAR sender[64];
            packet.Get_Wide_Terminated_String(sender, 64);
            Check(std::u16string(sender) == u"FixtureHost", "Wrong LAN announcer");
            const auto sequence = Field<uint32>(packet);
            Check(sequence > LastSequence, "LAN announcement sequence did not advance");
            LastSequence = sequence;
            const bool active = Field<bool>(packet);
            int count = 0;
            if (active) {
                Check(Field<int>(packet) == 2, "LAN announcement was not for C&C mode");
                count = Check_Game_Info(packet);
            }
            Check(packet.Get_Bits_Remaining() < 8, "Unexpected data in LAN announcement");
            if (active == hosting && count == players) {
                return;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("Timed out waiting for a LAN announcement");
    }

private:
    SOCKET Socket = INVALID_SOCKET;
    uint32 LastSequence = 0;
};

std::pair<int, int> Read_Combat_Counts(cPacket &packet)
{
    const int kills = Field<int>(packet);
    const int deaths = Field<int>(packet);
    Check(kills >= 0 && deaths >= 0, "Invalid combat counts");
    return {kills, deaths};
}

void Read_Player_Counts(cPacket &packet, Join_Client_State &client, int object)
{
    client.PlayerScores[object] = Field<float>(packet);
    client.PlayerCredits[object] = Field<float>(packet);
    Check(std::isfinite(client.PlayerScores[object]) && std::isfinite(client.PlayerCredits[object]) &&
        client.PlayerCredits[object] >= 0, "Invalid score or credits");
    client.PlayerCounts[object] = Read_Combat_Counts(packet);
}

void Read_Team_Counts(cPacket &packet, Join_Client_State &client, int team)
{
    client.TeamCounts[team] = Read_Combat_Counts(packet);
    client.TeamScores[team] = Field<float>(packet);
}

void Read_Player(cPacket &packet, Join_Client_State &client, int object, bool creation)
{
    if (creation) {
        WCHAR name[64];
        packet.Get_Wide_Terminated_String(name, 64);
        const std::u16string nickname(name);
        Check(nickname == u"First" || nickname == u"Second" || nickname == u"Late", "Unexpected player name");
    }
    const int id = Field<int>(packet);
    Check(creation ? client.Players.emplace(id, object).second : client.Players.at(id) == object,
        "Incorrect player object identity");
    Check(Field<int>(packet) == 0, "Unexpected ladder points");
    const int team = Field<int>(packet);
    Check((team == PLAYERTYPE_GDI || team == PLAYERTYPE_NOD) && Field<int>(packet) == 100 &&
        Field<int>(packet) == 0, "Incorrect team, damage scale or ping");
    Field<bool>(packet); // In game.
    Check(!Field<bool>(packet) && Field<bool>(packet), "Incorrect player state");
    ULONG ranking;
    packet.Get_Bits(ranking, 32);
    packet.Get_Bits(ranking, 32);
    Read_Player_Counts(packet, client, object);
}

float Read_Join_Position_Y(cPacket &packet, uint32 map_crc)
{
    const unsigned bits = map_crc == NextMapCRC ? 10 : 8;
    const float extent = map_crc == NextMapCRC ? 61.0f : 21.0f;
    ULONG coordinate;
    packet.Get_Bits(coordinate, bits); // X.
    packet.Get_Bits(coordinate, bits);
    const auto y = -extent + static_cast<float>(coordinate) * (2 * extent / ((1u << bits) - 1));
    packet.Get_Bits(coordinate, bits); // Z.
    return y;
}

std::vector<HeadlessSpawnWeapon> Fixture_Weapons(int team, int charge = 620, bool repair_fixture = false)
{
    const HeadlessFireMode primary{0.5f, 0, 20};
    const HeadlessFireMode secondary{0.5f, 0, 7};
    std::vector<HeadlessSpawnWeapon> weapons{{610, 1, 12, 12, -1, 0.35f, primary, secondary},
        {600 + team, 2, 40, 40, 200, 0.35f, primary, secondary},
        {charge, 7, 1, 1, charge == 620 ? 0 : 1, 0.35f, {}, {}}};
    if (repair_fixture) {
        weapons.insert(weapons.begin() + 2, {626, 4, -1, -1, -1, 0, {}, {}});
    }
    return weapons;
}

void Read_Soldier_Frequent(cPacket &packet, Join_Client_State &client, int object)
{
    if (Field<bool>(packet)) {
        client.SeatedSoldiers.insert(object);
        Check(!Field<bool>(packet), "Mounted soldier should have no host bone");
        Read_Join_Position_Y(packet, client.MapCRC);
        ULONG controls, analog;
        packet.Get_Bits(controls, ControlClass::NUM_BOOLEAN_CONTINUOUS);
        packet.Get_Bits(analog, 32);
        Check(controls == 0 && analog == 0x80808080u, "Mounted soldier exported infantry controls");
        return;
    }
    client.SeatedSoldiers.erase(object);
    int id = 0;
    int rounds = 0;
    if (Field<bool>(packet)) {
        id = Field<int>(packet);
        rounds = Field<int>(packet);
        Check(((id == 610 || id == 626) && rounds == -1) || ((id == 600 || id == 601) && rounds >= 0 && rounds <= 240) ||
            (id == 620 && rounds >= 0 && rounds <= 1) || ((id == 624 || id == 625) && rounds >= 0 && rounds <= 2),
            "Incorrect equipped weapon or ammo count");
    }
    client.SelectedWeapon[object] = id;
    client.Rounds[object] = rounds;
    client.Y[object] = Read_Join_Position_Y(packet, client.MapCRC);
    ULONG state, bits;
    packet.Get_Bits(state, 5);
    packet.Get_Bits(bits, 9); // Human substate.
    Check(state == 0 || state == 5 || state == 7, "Unexpected soldier state");
    if (state == 5) {
        for (int axis = 0; axis < 3; ++axis) {
            Field<float>(packet);
        }
    }
    Check(!Field<bool>(packet) && !Field<bool>(packet), "Unexpected damage or host bone");
    Read_Join_Position_Y(packet, client.MapCRC); // Target position.
    packet.Get_Bits(bits, ControlClass::NUM_BOOLEAN_CONTINUOUS);
    client.FireControls[object] = bits;
    packet.Get_Bits(bits, 32); // Analog controls.
}

void Read_Vehicle(cPacket &packet, Join_Client_State &client, int object)
{
    char model[32];
    char animation[32];
    packet.Get_Terminated_String(model, sizeof(model), true);
    packet.Get_Terminated_String(animation, sizeof(animation), true);
    Check(std::string(model) == "TEST_CAR" && *animation == 0, "Wrong vehicle model or animation");
    Check(Field<int>(packet) == 0 && Field<int>(packet) == 0 && Field<int>(packet) == 3 &&
        Field<int>(packet) == 0 && Field<int>(packet) == 0, "Incorrect vehicle animation fields");
    const int team = Field<int>(packet);
    Check(!Field<bool>(packet) && !Field<bool>(packet), "Unexpected vehicle HUD or visibility flag");
    const int driver = Field<int>(packet);
    Check((driver == -1 && team == PLAYERTYPE_NEUTRAL) ||
        (team == PLAYERTYPE_NOD && std::any_of(client.Soldiers.begin(), client.Soldiers.end(), [=](const auto &soldier) {
            return soldier.second == driver;
        })), "Vehicle driver must refer to an introduced soldier");
    client.VehicleDriver[object] = driver;
    Check(Field<int>(packet) == -1 && !Field<bool>(packet), "Unexpected second occupant or delivery");
    Check(!Field<bool>(packet), "Live vehicle was marked as destroyed");
    ULONG health, shield, armor;
    packet.Get_Bits(health, 11);
    packet.Get_Bits(shield, 11);
    packet.Get_Bits(armor, 2);
    Check(health == 128 && shield == 51 && armor == client.ShieldArmorCode, "Wrong vehicle defense state");
    Check(Field<int>(packet) == 0 && !Field<bool>(packet), "Unexpected vehicle weapon or engine state");
    Check(std::abs(Read_Join_Position_Y(packet, client.MapCRC) - 2.0f) <= 0.11f, "Wrong vehicle position");
    ULONG bits;
    for (int axis = 0; axis < 4; ++axis) {
        packet.Get_Bits(bits, 12); // Quaternion.
        Check(bits == (axis == 3 ? 4095u : 2048u), "Wrong vehicle orientation");
    }
    for (int axis = 0; axis < 3; ++axis) {
        packet.Get_Bits(bits, 15);
        Check(bits == 16384u, "Parked vehicle has linear velocity");
    }
    for (int axis = 0; axis < 3; ++axis) {
        packet.Get_Bits(bits, 12);
        Check(bits == 2048u, "Parked vehicle has angular velocity");
    }
    Check(!Field<bool>(packet) && !Field<bool>(packet), "Unexpected vehicle gunner override or host bone");
    Read_Join_Position_Y(packet, client.MapCRC);
    packet.Get_Bits(bits, ControlClass::NUM_BOOLEAN_CONTINUOUS);
    Check(bits == 0, "Unexpected vehicle controls");
    packet.Get_Bits(bits, 32);
    Check(bits == 0x80808080u, "Unexpected vehicle analog axes");
}

void Read_Soldier_Occasional(cPacket &packet, Join_Client_State &client, int object, int team = -1, int charge = 620)
{
    Join_Client_State::Defense state{Field<bool>(packet)};
    packet.Get_Bits(state.Health, 11);
    packet.Get_Bits(state.Shield, 11);
    ULONG shield_type;
    packet.Get_Bits(shield_type, 2);
    Check(shield_type == client.ShieldArmorCode, "The original shield save ID did not map to the network index");
    const int count = Field<int>(packet);
    const auto weapons = Fixture_Weapons(team, charge, client.RepairFixture);
    Check(count == static_cast<int>(weapons.size()), "Wrong size of the soldier inventory");
    for (const auto &weapon : weapons) {
        const int id = Field<int>(packet);
        const int rounds = Field<int>(packet);
        if (team >= 0) {
            const int starting = weapon.Total_Rounds();
            const bool spent = client.JoiningActiveCombat && rounds >= 0 && rounds <= starting;
            Check(id == weapon.DefinitionId && (rounds == starting || spent),
                "Incorrect weapon or ammunition in soldier creation for preset " + std::to_string(id));
        }
    }
    client.Vitals[object] = state;
}

void Read_Building(cPacket &packet, Join_Client_State &client, int object, bool rare)
{
    ULONG code;
    if (rare) {
        client.BuildingDestroyed[object] = Field<bool>(packet);
        Check(!Field<bool>(packet), "Wrong fixture building power state");
        packet.Get_Bits(code, 4);
        Check(code >= 8 && code <= 14, "Invalid building damage state");
    }
    Join_Client_State::Defense vitals{Field<bool>(packet)};
    packet.Get_Bits(vitals.Health, 11);
    packet.Get_Bits(vitals.Shield, 11);
    packet.Get_Bits(code, 2);
    Check(code == client.ShieldArmorCode && vitals.Shield == 5, "Building damage changed the shield or armor");
    client.BuildingVitals[object] = vitals;
}

void Read_Charge_Rare(cPacket &packet, Join_Client_State &client, int object)
{
    char model[32];
    char animation[32];
    packet.Get_Terminated_String(model, sizeof(model), true);
    packet.Get_Terminated_String(animation, sizeof(animation), true);
    Check(std::string(model) == "charge" && *animation == 0 &&
        Field<int>(packet) == 0 && Field<int>(packet) == 0 && Field<int>(packet) == 3 &&
        Field<int>(packet) == 0 && Field<int>(packet) == 0 && Field<int>(packet) == PLAYERTYPE_NOD &&
        !Field<bool>(packet), "Invalid C4 model or team");
    const int ammo = Field<int>(packet);
    Check(ammo == 642 || ammo == 646 || ammo == 647, "Invalid C4 ammo preset");
    auto &charge = client.Charges[object];
    charge.Owner = Field<int>(packet);
    ULONG code;
    for (int axis = 0; axis < 3; ++axis) {
        packet.Get_Bits(code, 15); // Projectile velocity.
    }
    charge.Stuck = Field<bool>(packet);
    if (charge.Stuck) {
        Read_Join_Position_Y(packet, client.MapCRC);
        charge.MCT = Field<bool>(packet);
        Check(!Field<bool>(packet), "C4 unexpectedly attached to a soldier");
        charge.AttachedObject = Field<int>(packet);
        Check(!Field<bool>(packet), "C4 unexpectedly attached to an animated static object");
    }
}

void Read_Charge_Defense(cPacket &packet, Join_Client_State &client, int object)
{
    Check(!Field<bool>(packet), "C4 update had zero health");
    ULONG code;
    packet.Get_Bits(client.Charges.at(object).Health, 11);
    Check(client.Charges.at(object).Health <= 102, "C4 health exceeded its preset");
    packet.Get_Bits(code, 11);
    Check(code == 0, "Wrong shield strength on C4");
    packet.Get_Bits(code, 2);
    Check(code == 2, "Wrong shield index on C4");
}

void Receive_Join_Packet(cPacket &packet, Join_Client_State &client)
{
    const int object = Field<int>(packet);
    const auto dirty = Field<BYTE>(packet);
    const bool deleting = Field<bool>(packet);
    if (dirty == 0 && deleting) {
        client.Deleted.insert(object);
        client.Charges.erase(object);
        client.VehicleDriver.erase(object);
        client.SeatedSoldiers.erase(object);
        Check(std::none_of(client.VehicleDriver.begin(), client.VehicleDriver.end(), [=](const auto &vehicle) {
            return vehicle.second == object;
        }), "Driver was deleted before its seat was cleared");
        const auto old_soldier = std::find_if(client.Soldiers.begin(), client.Soldiers.end(), [object](const auto &entry) {
            return entry.second == object;
        });
        if (old_soldier != client.Soldiers.end()) {
            client.Soldiers.erase(old_soldier);
        }
        const auto building = std::find_if(client.Buildings.begin(), client.Buildings.end(), [object](const auto &entry) {
            return entry.second == object;
        });
        if (building != client.Buildings.end()) {
            client.Buildings.erase(building);
            client.BuildingVitals.erase(object);
            client.BuildingDestroyed.erase(object);
        }
        Check(packet.Is_Flushed(), "Unexpected data in object deletion");
        return;
    }
    if (client.Charges.count(object)) {
        Check(!deleting && (dirty == NetworkObjectClass::BIT_RARE || dirty == NetworkObjectClass::BIT_OCCASIONAL),
            "Incorrect C4 state update");
        if (dirty == NetworkObjectClass::BIT_RARE) {
            Read_Charge_Rare(packet, client, object);
        }
        Read_Charge_Defense(packet, client, object);
        Check(packet.Is_Flushed(), "Unexpected data in C4 state update");
        return;
    }
    if (client.BuildingVitals.count(object)) {
        Check(!deleting && (dirty == NetworkObjectClass::BIT_RARE || dirty == NetworkObjectClass::BIT_OCCASIONAL),
            "Incorrect building update");
        Read_Building(packet, client, object, dirty == NetworkObjectClass::BIT_RARE);
        Check(packet.Is_Flushed(), "Unexpected data in building update");
        return;
    }
    if (client.VehicleDriver.count(object)) {
        Check(!deleting && dirty == NetworkObjectClass::BIT_RARE, "Incorrect vehicle state update");
        Read_Vehicle(packet, client, object);
        Check(packet.Is_Flushed(), "Unexpected data in vehicle state update");
        return;
    }
    if (dirty == NetworkObjectClass::BIT_OCCASIONAL && client.PlayerCounts.count(object)) {
        Read_Player_Counts(packet, client, object);
        Check(!deleting && packet.Is_Flushed(), "Incorrect cPlayer statistics update");
        return;
    }
    if (dirty == NetworkObjectClass::BIT_OCCASIONAL && client.TeamObjects.count(object)) {
        client.TeamScores[client.TeamObjects.at(object)] = Field<float>(packet);
        Check(!deleting && packet.Is_Flushed(), "Incorrect cTeam score update");
        return;
    }
    if (dirty == NetworkObjectClass::BIT_FREQUENT || dirty == NetworkObjectClass::BIT_OCCASIONAL) {
        Check(!deleting, "Unexpected deletion flag on soldier update");
        if (dirty == NetworkObjectClass::BIT_OCCASIONAL) {
            Read_Soldier_Occasional(packet, client, object);
        }
        Read_Soldier_Frequent(packet, client, object);
        Check(packet.Is_Flushed(), "Unexpected data in soldier update");
        return;
    }
    if (dirty == NetworkObjectClass::BIT_RARE) {
        if (const auto team = client.TeamObjects.find(object); team != client.TeamObjects.end()) {
            Read_Team_Counts(packet, client, team->second);
        } else {
            Read_Player(packet, client, object, false);
        }
        Check(!deleting && packet.Is_Flushed(), "Incorrect cTeam or cPlayer update");
        return;
    }
    Check(dirty == 15, "Unexpected object update in the join sequence");
    switch (Field<int>(packet)) {
    case 1010: { // cTeam.
        const int team = Field<int>(packet);
        Check(!deleting && client.Teams.insert(team).second && client.TeamObjects.emplace(object, team).second,
            "Incorrect team identity");
        Read_Team_Counts(packet, client, team);
        break;
    }
    case 1002: { // cPlayerKill.
        const int killer = Field<int>(packet);
        const int victim = Field<int>(packet);
        Check(deleting && client.Players.count(killer) && client.Players.count(victim), "Invalid kill message");
        client.KillMessages.emplace_back(killer, victim);
        break;
    }
    case 1008: { // cGameOptionsEvent.
        Check(client.Teams == std::set<int>{PLAYERTYPE_NOD, PLAYERTYPE_GDI}, "Teams must arrive before game options");
        Check_Game_Info(packet);
        Check(Field<int>(packet) == 23 && Field<int>(packet) == 1 &&
            Field<int>(packet) == 10 && Field<int>(packet) == 0, "Wrong second-tier game settings");
        ULONG flags;
        packet.Get_Bits(flags, 7);
        client.CanRepairBuildings = (flags & 4) != 0;
        WCHAR motd[100];
        packet.Get_Wide_Terminated_String(motd, 100, true);
        Check(std::u16string(motd) == u"Ready", "Wrong message of the day in game options");
        client.BaseDestructionEndsGame = Field<bool>(packet);
        Check(!Field<bool>(packet) && Field<int>(packet) == 0, "Incorrect beacon option or starting credits");
        const float remaining = Field<float>(packet);
        Check(remaining > 0 && remaining <= 1380.0f &&
            Field<int>(packet) == 1 && Field<uint32>(packet) == 0 && Field<uint32>(packet) == TestMapCRC,
            "The original game options packet had an incorrect footer");
        client.OptionsTime = remaining;
        client.MapCRC = TestMapCRC;
        break;
    }
    case 1011: // cPlayer.
        Check(!deleting, "The player object was immediately deleted");
        Read_Player(packet, client, object, true);
        break;
    case 1000: { // GameObj with factory data and all update tiers.
        const int preset = Field<int>(packet);
        if (preset == 310) {
            Check(!deleting && !client.VehicleDriver.count(object), "Invalid vehicle creation");
            Read_Join_Position_Y(packet, client.MapCRC);
            Check(Field<float>(packet) == 0.0f && Field<int>(packet) == -99999 && Field<int>(packet) == 0,
                "Vehicle creation must carry facing, server ownership and the lock owner");
            Read_Vehicle(packet, client, object);
            break;
        }
        if (preset == 621) {
            Check(!deleting, "C4 deleted on creation");
            Read_Join_Position_Y(packet, client.MapCRC);
            Check(std::isfinite(Field<float>(packet)), "Invalid C4 facing");
            Read_Charge_Rare(packet, client, object);
            Read_Charge_Defense(packet, client, object);
            break;
        }
        if (preset == 300 || preset == 301) {
            Check(!deleting && client.Buildings.emplace(preset, object).second, "Invalid building creation");
            Read_Join_Position_Y(packet, client.MapCRC);
            Read_Join_Position_Y(packet, client.MapCRC);
            ULONG radius;
            packet.Get_Bits(radius, 9);
            Check(radius == 51, "Building creation lost the collection sphere");
            Read_Building(packet, client, object, true);
            break;
        }
        const bool engineer = preset == 402 || preset == 403;
        const bool technician = preset == 404 || preset == 405;
        const int first_preset = client.MapCRC == NextMapCRC ? 410 : (technician ? 404 : (engineer ? 402 : 400));
        client.Y[object] = Read_Join_Position_Y(packet, client.MapCRC);
        const float facing = Field<float>(packet);
        const int owner = Field<int>(packet);
        Check(facing == 0 && client.Players.count(owner) == 1 && client.Soldiers.emplace(owner, object).second,
            "A soldier must follow the owning player object");
        char model[32];
        char animation[32];
        packet.Get_Terminated_String(model, sizeof(model));
        packet.Get_Terminated_String(animation, sizeof(animation), true);
        Check((preset == first_preset || preset == first_preset + 1) &&
            std::string(model) == (preset == first_preset ? "soldier0" : "soldier1") && *animation == 0,
            "The soldier did not use the current map's preset");
        Check(Field<int>(packet) == 0 && Field<int>(packet) == 0 && Field<int>(packet) == 3 &&
            Field<int>(packet) == 0 && Field<int>(packet) == 0 && Field<int>(packet) == preset - first_preset &&
            !Field<bool>(packet) && Field<int>(packet) == preset,
            "Incorrect soldier rare fields");
        Read_Soldier_Occasional(packet, client, object, preset - first_preset, technician ? 625 : (engineer ? 624 : 620));
        const auto &defense = client.Vitals.at(object);
        Check(!defense.Dead && defense.Health == 102 && defense.Shield == 51, "Incorrect spawn vitals");
        Read_Soldier_Frequent(packet, client, object);
        Check(client.JoiningActiveCombat || client.SeatedSoldiers.count(object) ||
            client.SelectedWeapon.at(object) == 600 + preset - first_preset,
            "The primary weapon must start equipped");
        break;
    }
    case 1012: { // cGameDataUpdateEvent.
        const int remaining = Field<int>(packet);
        const float limit = client.Round == 1 ? client.OptionsTime.value() : 1380.0f;
        Check(remaining > 0 && remaining <= limit && Field<int>(packet) == client.Round && !client.Soldiers.empty(),
            "Incorrect game clock after map load");
        client.GameDataTime = remaining;
        break;
    }
    case 1014: { // cScExplosionEvent.
        const int effect = Field<int>(packet);
        Check(deleting && (effect == 643 || effect == 649), "Incorrect explosion definition");
        Read_Join_Position_Y(packet, client.MapCRC);
        const int owner = Field<int>(packet);
        (effect == 649 ? client.DisarmOwners : client.ExplosionOwners).push_back(owner);
        break;
    }
    case 1003: { // cWinEvent.
        client.LastWinner = Field<int>(packet);
        Check(deleting && (client.LastWinner == PLAYERTYPE_NOD || client.LastWinner == PLAYERTYPE_GDI) &&
            Field<int>(packet) == -99999 &&
            Field<int>(packet) == client.Round, "Incorrect round result");
        client.CycleEnded = Field<bool>(packet);
        client.LastWinReason = Field<int>(packet);
        client.LastWinDuration = Field<uint32>(packet);
        Check((client.LastWinReason == 2 && client.LastWinDuration == 1380) ||
              (client.LastWinReason == 3 && client.LastWinDuration < 1380),
            "Incorrect reason or duration for round end");
        WCHAR mvp[64];
        packet.Get_Wide_Terminated_String(mvp, 64, true);
        Check(*mvp == 0 && Field<int>(packet) == 0 && Field<uint32>(packet) == 0, "Incorrect MVP or mod data");
        client.MapCRC = Field<uint32>(packet);
        ++client.Round;
        client.GameDataTime.reset();
        break;
    }
    default:
        throw std::runtime_error("Unexpected class in original join sequence");
    }
    Check(packet.Is_Flushed(), "An initial object did not match the original packet layout");
}

void Receive_First_Join(cPacket &packet) { Receive_Join_Packet(packet, FirstJoinClient); }
void Receive_Second_Join(cPacket &packet) { Receive_Join_Packet(packet, SecondJoinClient); }
void Receive_Late_Join(cPacket &packet) { Receive_Join_Packet(packet, LateJoinClient); }
void Receive_Loading_Join(cPacket &packet) { Receive_Join_Packet(packet, LoadingJoinClient); }

template<class CheckReady>
void Pump_Clients(std::initializer_list<cConnection *> connections, CheckReady ready)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        for (auto *connection : connections) {
            connection->Service_Read();
            connection->Service_Send(true);
        }
        if (ready()) {
            return;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Timed out waiting for the original FDS join sequence");
}

void Connect_Join_Client(cConnection &client, const WCHAR *name, const WCHAR *password, uint32 key,
    Client_Packet_Handler handler)
{
    client.Init_As_Client(htonl(INADDR_LOOPBACK), JoinPort);
    client.Set_Bandwidth_Budget_Out(TestBandwidth);
    client.Install_Accept_Handler([] {});
    client.Install_Refusal_Handler(Refused);
    client.Install_Client_Packet_Handler(handler);
    cPacket request;
    request.Add_Wide_Terminated_String(name);
    request.Add_Wide_Terminated_String(password, true);
    request.Add(key);
    request.Add(TestBandwidth);
    client.Connect_Cs(request);
}

void Send_Bio(cConnection &client, const WCHAR *name, int team, int id_offset, const char *map)
{
    const int owner = client.Get_Local_Id();
    cPacket packet;
    packet.Add(FirstControlObject + (owner - 1) * 100000 + id_offset);
    packet.Add(static_cast<BYTE>(15));
    packet.Add(false);
    packet.Add(1026); // NETCLASSID_BIOEVENT.
    packet.Add(owner);
    packet.Add_Wide_Terminated_String(name);
    packet.Add(team);
    packet.Add(0u); // Clan ID.
    packet.Add_Terminated_String(map);
    client.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, SEND_RELIABLE);
}

void Send_Loading(cConnection &client, int id_offset, bool loading)
{
    const int owner = client.Get_Local_Id();
    cPacket packet;
    packet.Add(FirstControlObject + (owner - 1) * 100000 + id_offset);
    packet.Add(static_cast<BYTE>(15));
    packet.Add(false);
    packet.Add(1027); // NETCLASSID_LOADINGEVENT.
    packet.Add(owner);
    packet.Add(loading);
    client.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, SEND_RELIABLE);
}

void Check_Vehicle_Seats(const HeadlessCollision &collision, const HeadlessJoinData &data)
{
    FirstJoinClient = {};
    SecondJoinClient = {};
    LateJoinClient = {};
    const std::vector<HeadlessMap> maps{{"C&C_Test.mix", collision, data}};
    HeadlessJoinOptions options;
    options.Owner = u"FixtureHost";
    options.Title = u"Join fixture";
    options.Motd = u"Ready";
    options.Password = u"testpass";
    options.Address = htonl(INADDR_LOOPBACK);
    options.TimeLimit = 23;
    cConnection server;
    Check(server.Init_As_Server(0, 16, true, INADDR_LOOPBACK), "Could not bind the vehicle test server");
    server.Set_Bandwidth_Budget_Out(TestBandwidth);
    JoinPort = server.Get_Local_Port();
    JoinKey = data.VersionKey;
    HeadlessPlayerSession session(maps.front().Collision, server);
    HeadlessJoin join(server, session, maps, options);
    server.Install_Application_Acceptance_Handler([&](cPacket &packet) { return join.Accept(packet); });
    server.Install_Conn_Handler([&](int id) { join.Connected(id); });
    int received = 0;
    server.Install_Server_Packet_Handler([&](cPacket &packet, int id) {
        join.Receive(packet, id);
        ++received;
    });

    cConnection first;
    cConnection second;
    cConnection late;
    const auto pump = [&](const auto &ready) {
        Pump_Clients({&first, &second, &late}, [&] {
            server.Service_Read();
            server.Service_Send(true);
            return ready();
        });
    };
    Connect_Join_Client(first, u"First", u"testpass", JoinKey, Receive_First_Join);
    Connect_Join_Client(second, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    pump([&] { return FirstJoinClient.OptionsTime && SecondJoinClient.OptionsTime; });
    Send_Bio(first, u"First", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    Send_Bio(second, u"Second", PLAYERTYPE_GDI, 1, "C&C_Test.mix");
    pump([&] { return FirstJoinClient.Soldiers.size() == 2 && SecondJoinClient.GameDataTime; });
    const int driver = FirstJoinClient.Soldiers.at(1);
    const int distant = FirstJoinClient.Soldiers.at(2);
    Check(FirstJoinClient.VehicleDriver.size() == 1, "Placed vehicle did not join the map");
    const int vehicle = FirstJoinClient.VehicleDriver.begin()->first;
    const auto use = [&](cConnection &client, int soldier, bool create) {
        const int peer = client.Get_Local_Id();
        const int before = received;
        cPacket packet;
        Write_Movement(packet, FirstControlObject + (peer - 1) * 100000, create ? peer : 0, soldier,
            {0, -8, 1.62f}, 21.0f, 8, 1u << ControlClass::BOOLEAN_ACTION, 0, false);
        client.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, create ? SEND_RELIABLE : SEND_UNRELIABLE);
        pump([&] { return received > before; });
        session.Advance(100ms);
    };
    use(second, distant, true);
    pump([&] { return SecondJoinClient.Y.count(distant); });
    Check(SecondJoinClient.VehicleDriver.at(vehicle) == -1, "Distant player entered a vehicle");
    use(first, driver, true);
    pump([&] { return FirstJoinClient.VehicleDriver.at(vehicle) == driver &&
        SecondJoinClient.VehicleDriver.at(vehicle) == driver; });

    Connect_Join_Client(late, u"Late", u"testpass", JoinKey, Receive_Late_Join);
    pump([&] { return LateJoinClient.OptionsTime.has_value(); });
    Send_Bio(late, u"Late", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    pump([&] { return LateJoinClient.GameDataTime && LateJoinClient.VehicleDriver.count(vehicle); });
    Check(LateJoinClient.VehicleDriver.at(vehicle) == driver && LateJoinClient.SeatedSoldiers.count(driver),
        "A later client did not receive the occupied driver seat");
    const int contender = LateJoinClient.Soldiers.at(3);
    use(late, contender, true);
    pump([&] { return LateJoinClient.Y.count(contender); });
    Check(LateJoinClient.VehicleDriver.at(vehicle) == driver, "A second soldier took an occupied driver seat");

    use(first, driver, false);
    pump([&] { return LateJoinClient.VehicleDriver.at(vehicle) == -1 &&
        std::abs(LateJoinClient.Y.at(driver) - 1.35f) <= 0.13f; });
    use(late, contender, false);
    pump([&] { return SecondJoinClient.VehicleDriver.at(vehicle) == contender; });
    join.Disconnected(3);
    pump([&] { return SecondJoinClient.Deleted.count(contender) && SecondJoinClient.VehicleDriver.at(vehicle) == -1; });
    session.Reset_Map(maps.front().Collision);
    pump([&] { return SecondJoinClient.Deleted.count(vehicle); });
}

bool Has_Combat_Counts(const Join_Client_State &client, int killer, int victim, int killer_team, int victim_team)
{
    using Counts = std::pair<int, int>;
    return client.PlayerCounts.at(client.Players.at(killer)) == Counts{1, 0} &&
        client.PlayerCounts.at(client.Players.at(victim)) == Counts{0, 1} &&
        client.TeamCounts.at(killer_team) == Counts{1, killer_team == victim_team ? 1 : 0} &&
        client.TeamCounts.at(victim_team) == Counts{killer_team == victim_team ? 1 : 0, 1} &&
        client.KillMessages == std::vector<Counts>{{killer, victim}};
}

void Check_Building_Combat(const HeadlessCollision &collision, bool ends_game,
    const HeadlessJoinData *charge_data = nullptr, bool remote = false)
{
    FirstJoinClient = {};
    SecondJoinClient = {};
    LateJoinClient = {};
    HeadlessJoinData data{42, 1, 2, {{17, {{0.5f, 0}}}, {23, {{2, 0}}}},
        {{{400, "soldier0", 100, 50, 1, Fixture_Weapons(0), 600},
          {401, "soldier1", 100, 50, 1, Fixture_Weapons(1), 601}}}};
    if (charge_data) {
        data = *charge_data;
    }
    data.Soldiers[PLAYERTYPE_NOD].Weapons[1].PrimaryFire->Bullet = HeadlessBulletDefinition{30, 0, 20, 400, 0};
    const std::vector<HeadlessMap> maps{{"C&C_Test.mix", collision, data}};
    HeadlessJoinOptions options;
    options.Owner = u"FixtureHost";
    options.Title = u"Join fixture";
    options.Motd = u"Ready";
    options.Password = u"testpass";
    options.Address = htonl(INADDR_LOOPBACK);
    options.TimeLimit = 23;
    options.BaseDestructionEndsGame = ends_game;
    cConnection server;
    Check(server.Init_As_Server(0, 16, true, INADDR_LOOPBACK), "Could not bind the building test server");
    server.Set_Bandwidth_Budget_Out(TestBandwidth);
    JoinPort = server.Get_Local_Port();
    JoinKey = data.VersionKey;
    HeadlessPlayerSession session(maps.front().Collision, server);
    HeadlessJoin join(server, session, maps, options);
    const auto round_time = HeadlessJoin::Clock::now() + 35s;
    server.Install_Application_Acceptance_Handler([&](cPacket &packet) { return join.Accept(packet); });
    server.Install_Conn_Handler([&](int id) { join.Connected(id); });
    int received = 0;
    server.Install_Server_Packet_Handler([&](cPacket &packet, int id) {
        join.Receive(packet, id);
        ++received;
    });

    cConnection first;
    cConnection second;
    cConnection late;
    const auto pump = [&](const auto &ready) {
        Pump_Clients({&first, &second, &late}, [&] {
            server.Service_Read();
            server.Service_Send(true);
            return ready();
        });
    };
    Connect_Join_Client(first, u"First", u"testpass", JoinKey, Receive_First_Join);
    Connect_Join_Client(second, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    pump([&] { return FirstJoinClient.OptionsTime && SecondJoinClient.OptionsTime; });
    Check(FirstJoinClient.BaseDestructionEndsGame == ends_game && SecondJoinClient.BaseDestructionEndsGame == ends_game,
        "The building victory setting did not reach both clients");
    Send_Bio(first, u"First", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    Send_Bio(second, u"Second", PLAYERTYPE_GDI, 1, "C&C_Test.mix");
    pump([&] { return FirstJoinClient.GameDataTime && SecondJoinClient.GameDataTime; });
    Check(FirstJoinClient.Buildings.size() == 2 && FirstJoinClient.Buildings == SecondJoinClient.Buildings,
        "Both clients must load the same buildings");
    const auto buildings = FirstJoinClient.Buildings;
    const int plant = buildings.at(300);
    const int terminal = buildings.at(301);
    int soldier = FirstJoinClient.Soldiers.at(1);
    const int player = FirstJoinClient.Players.at(1);
    const auto health_code = [](float health) { return static_cast<ULONG>(std::round(health * 2047 / 2000)); };
    Check(FirstJoinClient.BuildingVitals.at(plant).Health == health_code(30) &&
        FirstJoinClient.BuildingVitals.at(terminal).Health == health_code(80), "Incorrect initial building health");
    const auto advance = [&](int ticks = 12) {
        for (int tick = 0; tick < ticks; ++tick) {
            session.Advance(100ms);
            join.Update_Combat();
        }
    };
    advance();
    Check(join.Update_Round(round_time), "Round clock stopped before building combat");
    const auto controls = [&](int owner, const Vector3 &aim, ULONG buttons, ULONG trigger) {
        const int before = received;
        cPacket packet;
        Write_Movement(packet, FirstControlObject, owner, soldier, aim, 21.0f, 8, buttons, trigger, false);
        first.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, owner ? SEND_RELIABLE : SEND_UNRELIABLE);
        pump([&] { return received > before; });
    };
    const auto fire = [&](int owner, const Vector3 &aim, int object, float health, float points, bool round_ends = false) {
        controls(owner, aim, 0, 1);
        advance();
        const auto updated = [&](const Join_Client_State &client) {
            return client.BuildingVitals.at(object).Health == health_code(health) &&
                client.BuildingDestroyed.at(object) == (health == 0) && client.PlayerScores.at(player) == points &&
                client.PlayerCredits.at(player) == points && client.Round == (round_ends ? 2 : 1);
        };
        pump([&] { return updated(FirstJoinClient) && updated(SecondJoinClient); });
    };
    fire(1, {8, 0, 1.62f}, plant, 15, 15);
    fire(0, {8, 0, 1.62f}, plant, 0, 40);
    if (!charge_data) {
        fire(0, {0, -8, 1.62f}, terminal, 20, 100);
        fire(0, {0, -8, 1.62f}, terminal, 0, 130, ends_game);
    } else {
        const Vector3 aim(0, -8, 1.62f);
        constexpr ULONG select_charge = 1u << ControlClass::BOOLEAN_SELECT_WEAPON_7;
        constexpr ULONG use_weapon = 1u << ControlClass::BOOLEAN_WEAPON_USE;
        const int charge_weapon = remote ? 624 : 620;
        const int charge_count = remote ? 2 : 1;
        controls(0, aim, select_charge | (remote ? 0 : use_weapon), 0);
        advance();
        pump([&] { return FirstJoinClient.SelectedWeapon.at(soldier) == charge_weapon &&
            SecondJoinClient.SelectedWeapon.at(soldier) == charge_weapon; });
        for (int index = 0; index < charge_count; ++index) {
            if (index > 0) {
                advance(); // Reload the engineer's second charge.
            }
            controls(0, aim, 0, 1);
            advance(3);
            const auto placed = [&](const Join_Client_State &client) {
                return client.Charges.size() == static_cast<std::size_t>(index + 1) &&
                    std::all_of(client.Charges.begin(), client.Charges.end(), [](const auto &entry) {
                        return entry.second.Stuck;
                    }) && client.Rounds.at(soldier) == charge_count - index - 1;
            };
            pump([&] { return placed(FirstJoinClient) && placed(SecondJoinClient); });
        }
        const auto charges = FirstJoinClient.Charges;

        LateJoinClient.JoiningActiveCombat = true;
        Connect_Join_Client(late, u"Late", u"testpass", JoinKey, Receive_Late_Join);
        pump([&] { return LateJoinClient.OptionsTime.has_value(); });
        Send_Bio(late, u"Late", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
        pump([&] { return LateJoinClient.GameDataTime && LateJoinClient.Charges.size() == charges.size(); });
        for (const auto *client : {&FirstJoinClient, &SecondJoinClient, &LateJoinClient}) {
            for (const auto &[id, state] : client->Charges) {
                Check(charges.count(id) && state.Owner == soldier && state.MCT && state.AttachedObject == terminal,
                    "C4 did not stick to the terminal for all clients");
            }
        }
        if (remote) {
            session.Advance(35s);
            join.Update_Combat();
            const auto other_controls = [&](int owner, ULONG buttons, ULONG trigger) {
                const int before = received;
                cPacket packet;
                Write_Movement(packet, FirstControlObject + 100000, owner, SecondJoinClient.Soldiers.at(2),
                    aim, 21.0f, 8, buttons, trigger, false);
                second.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, owner ? SEND_RELIABLE : SEND_UNRELIABLE);
                pump([&] { return received > before; });
            };
            other_controls(2, select_charge, 0);
            advance();
            other_controls(0, use_weapon, 2); // Use and held secondary fire accompany the same button press.
            advance();
            controls(0, aim, (1u << ControlClass::BOOLEAN_SELECT_WEAPON_2) | use_weapon, 2);
            advance();
        } else {
            advance(11);
        }
        pump([&] { return true; });
        Check(FirstJoinClient.ExplosionOwners.empty() && SecondJoinClient.ExplosionOwners.empty() &&
            LateJoinClient.ExplosionOwners.empty() && FirstJoinClient.BuildingVitals.at(terminal).Health == health_code(80),
            "C4 detonated without its trigger");
        if (remote) {
            controls(0, aim, select_charge | use_weapon, 2);
        }
        advance(3);
        pump([&] {
            return FirstJoinClient.Round == 2 && SecondJoinClient.Round == 2 && LateJoinClient.Round == 2;
        });
        for (const auto *client : {&FirstJoinClient, &SecondJoinClient, &LateJoinClient}) {
            const bool deleted = std::all_of(charges.begin(), charges.end(), [&](const auto &entry) {
                return client->Deleted.count(entry.first) != 0;
            });
            Check(client->ExplosionOwners == std::vector<int>(charge_count, soldier) && deleted &&
                client->Charges.empty() && client->BuildingDestroyed.at(terminal) &&
                client->Vitals.at(soldier).Health == health_code(remote ? 55 : 78) &&
                client->Vitals.at(client->Soldiers.at(3)).Health == health_code(100),
                "C4 damage, owner attribution, team protection or deletion did not reach all clients");
            Check(client->PlayerScores.at(player) == 130 && client->PlayerCredits.at(player) == 130,
                "C4 building damage did not award damage and destruction points");
        }
    }

    for (const auto *client : {&FirstJoinClient, &SecondJoinClient}) {
        Check(client->TeamScores.at(PLAYERTYPE_NOD) == (ends_game ? 5130 : 130) &&
            client->TeamScores.at(PLAYERTYPE_GDI) == 0 && client->KillMessages.empty() &&
            client->PlayerCounts.at(player) == std::pair<int, int>{0, 0},
            "Building combat yielded incorrect score or infantry statistics");
        Check(client->LastWinReason == (ends_game ? 3 : 0) && client->LastWinner == (ends_game ? PLAYERTYPE_NOD : -1) &&
            client->LastWinDuration == (ends_game ? 35u : 0u), "Incorrect base destruction result or round duration");
    }
    if (ends_game) {
        Check(join.Update_Round(round_time + 11s), "The round did not restart after base destruction");
        Send_Loading(first, 2, true);
        Send_Loading(second, 2, true);
        Send_Loading(first, 3, false);
        Send_Loading(second, 3, false);
        pump([&] { return FirstJoinClient.GameDataTime && SecondJoinClient.GameDataTime; });
        Check(FirstJoinClient.Buildings.size() == 2 && FirstJoinClient.Buildings == SecondJoinClient.Buildings &&
            FirstJoinClient.Deleted.count(plant) && FirstJoinClient.Deleted.count(terminal) &&
            FirstJoinClient.BuildingVitals.at(FirstJoinClient.Buildings.at(300)).Health == health_code(30) &&
            FirstJoinClient.BuildingVitals.at(FirstJoinClient.Buildings.at(301)).Health == health_code(80),
            "The next round did not restore both buildings");
        if (remote) {
            soldier = FirstJoinClient.Soldiers.at(1);
            const Vector3 aim(0, -8, 1.62f);
            controls(0, aim, 1u << ControlClass::BOOLEAN_SELECT_WEAPON_7, 0);
            advance();
            controls(0, aim, 0, 1);
            advance(3);
            pump([&] { return SecondJoinClient.Charges.size() == 1; });
            const auto charge = SecondJoinClient.Charges.begin()->first;
            join.Disconnected(1);
            pump([&] { return SecondJoinClient.Deleted.count(charge); });
            Check(SecondJoinClient.ExplosionOwners.size() == 2 && SecondJoinClient.Charges.empty(),
                "The remote charge did not disarm when its owner left");
        }
    }
}

void Check_Proximity_Combat(const HeadlessCollision &collision, const HeadlessJoinData &data)
{
    FirstJoinClient = {};
    SecondJoinClient = {};
    LateJoinClient = {};
    LateJoinClient.JoiningActiveCombat = true;
    const std::vector<HeadlessMap> maps{{"C&C_Test.mix", collision, data}};
    HeadlessJoinOptions options;
    options.Owner = u"FixtureHost";
    options.Title = u"Join fixture";
    options.Motd = u"Ready";
    options.Password = u"testpass";
    options.Address = htonl(INADDR_LOOPBACK);
    options.TimeLimit = 23;
    options.FriendlyFire = true;
    cConnection server;
    Check(server.Init_As_Server(0, 16, true, INADDR_LOOPBACK), "Could not bind the proximity mine test server");
    server.Set_Bandwidth_Budget_Out(TestBandwidth);
    JoinPort = server.Get_Local_Port();
    JoinKey = data.VersionKey;
    HeadlessPlayerSession session(maps.front().Collision, server);
    HeadlessJoin join(server, session, maps, options);
    server.Install_Application_Acceptance_Handler([&](cPacket &packet) { return join.Accept(packet); });
    server.Install_Conn_Handler([&](int id) { join.Connected(id); });
    int received = 0;
    server.Install_Server_Packet_Handler([&](cPacket &packet, int id) {
        join.Receive(packet, id);
        ++received;
    });

    cConnection first;
    cConnection second;
    cConnection late;
    const auto pump = [&](const auto &ready) {
        Pump_Clients({&first, &second, &late}, [&] {
            server.Service_Read();
            server.Service_Send(true);
            return ready();
        });
    };
    const auto advance = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            session.Advance(100ms);
            join.Update_Combat();
        }
    };
    Connect_Join_Client(first, u"First", u"testpass", JoinKey, Receive_First_Join);
    Connect_Join_Client(second, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    pump([&] { return FirstJoinClient.OptionsTime && SecondJoinClient.OptionsTime; });
    Send_Bio(first, u"First", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    Send_Bio(second, u"Second", PLAYERTYPE_GDI, 1, "C&C_Test.mix");
    pump([&] { return FirstJoinClient.Soldiers.size() == 2 && SecondJoinClient.GameDataTime; });
    const int owner = FirstJoinClient.Soldiers.at(1);
    const int enemy = FirstJoinClient.Soldiers.at(2);
    const int terminal = FirstJoinClient.Buildings.at(301);
    const auto controls = [&](cConnection &client, int soldier, bool create, const Vector3 &aim,
                              ULONG buttons, ULONG trigger, bool moving) {
        const int peer = client.Get_Local_Id();
        const int before = received;
        cPacket packet;
        Write_Movement(packet, FirstControlObject + (peer - 1) * 100000, create ? peer : 0, soldier,
            aim, 21.0f, 8, buttons, trigger, moving);
        client.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, create ? SEND_RELIABLE : SEND_UNRELIABLE);
        pump([&] { return received > before; });
    };
    constexpr ULONG select_mine = 1u << ControlClass::BOOLEAN_SELECT_WEAPON_7;
    constexpr ULONG use_weapon = 1u << ControlClass::BOOLEAN_WEAPON_USE;
    const Vector3 aim(0, -8, 1.62f);
    controls(first, owner, true, aim, select_mine | use_weapon, 2, false); // Select the second proximity mode.
    advance(12);
    pump([&] { return FirstJoinClient.SelectedWeapon.at(owner) == 625 &&
        SecondJoinClient.SelectedWeapon.at(owner) == 625; });
    Check(FirstJoinClient.Rounds.at(owner) == 2, "The technician did not receive the original mine grant");
    controls(first, owner, false, aim, 0, 1, false);
    advance(3);
    pump([&] {
        return FirstJoinClient.Charges.size() == 1 && FirstJoinClient.Charges.begin()->second.Stuck &&
            FirstJoinClient.Rounds.at(owner) == 1 && SecondJoinClient.Charges.size() == 1;
    });
    const int mine = FirstJoinClient.Charges.begin()->first;
    Check(FirstJoinClient.Charges.at(mine).MCT && FirstJoinClient.Charges.at(mine).AttachedObject == terminal,
        "The proximity mine did not attach to the terminal");

    Connect_Join_Client(late, u"Late", u"testpass", JoinKey, Receive_Late_Join);
    pump([&] { return LateJoinClient.OptionsTime.has_value(); });
    Send_Bio(late, u"Late", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    pump([&] { return LateJoinClient.GameDataTime && LateJoinClient.Charges.count(mine); });
    const int ally = LateJoinClient.Soldiers.at(3);
    const auto still_placed = [&] {
        pump([&] { return true; });
        for (const auto *client : {&FirstJoinClient, &SecondJoinClient, &LateJoinClient}) {
            Check(client->ExplosionOwners.empty() && client->Charges.count(mine),
                "The proximity mine detonated before its trigger conditions were met");
        }
    };

    controls(second, enemy, true, {-8, 0, 0}, 0, 0, true);
    advance(2); // The enemy enters the selected radius before the mine is armed.
    still_placed();
    controls(second, enemy, false, {8, 0, 0}, 0, 0, true);
    advance(2); // The enemy retreats before the first scan.
    controls(second, enemy, false, {8, 0, 0}, 0, 0, false);
    controls(first, owner, false, aim, use_weapon, 2, false);
    advance(7); // The mine is now armed, with only teammates in range.
    still_placed();

    controls(second, enemy, false, {-8, 0, 0}, 0, 0, true);
    advance(6);
    pump([&] {
        return FirstJoinClient.Deleted.count(mine) && SecondJoinClient.Deleted.count(mine) &&
            LateJoinClient.Deleted.count(mine) && FirstJoinClient.BuildingDestroyed.at(terminal) &&
            SecondJoinClient.Vitals.at(enemy).Health < 102 && LateJoinClient.Vitals.at(ally).Health < 102;
    });
    for (const auto *client : {&FirstJoinClient, &SecondJoinClient, &LateJoinClient}) {
        Check(client->ExplosionOwners == std::vector<int>{owner} && client->Charges.empty() &&
            client->BuildingDestroyed.at(terminal), "The proximity mine did not detonate once for all clients");
    }
}

void Check_Repair_Combat(const HeadlessCollision &collision, HeadlessJoinData data, bool can_repair)
{
    FirstJoinClient = {};
    SecondJoinClient = {};
    for (auto *client : {&FirstJoinClient, &SecondJoinClient}) {
        client->RepairFixture = true;
        client->ShieldArmorCode = 1;
    }
    data.Soldiers[PLAYERTYPE_NOD].Weapons[1].PrimaryFire->Bullet = HeadlessBulletDefinition{30, 0, 20, 400, 0};
    const std::vector<HeadlessMap> maps{{"C&C_Test.mix", collision, std::move(data)}};
    HeadlessJoinOptions options;
    options.Owner = u"FixtureHost";
    options.Title = u"Join fixture";
    options.Motd = u"Ready";
    options.Password = u"testpass";
    options.Address = htonl(INADDR_LOOPBACK);
    options.TimeLimit = 23;
    options.CanRepairBuildings = can_repair;
    cConnection server;
    Check(server.Init_As_Server(0, 16, true, INADDR_LOOPBACK), "Could not bind the repair test server");
    server.Set_Bandwidth_Budget_Out(TestBandwidth);
    JoinPort = server.Get_Local_Port();
    JoinKey = maps.front().Join.VersionKey;
    HeadlessPlayerSession session(maps.front().Collision, server);
    HeadlessJoin join(server, session, maps, options);
    server.Install_Application_Acceptance_Handler([&](cPacket &packet) { return join.Accept(packet); });
    server.Install_Conn_Handler([&](int id) { join.Connected(id); });
    int received = 0;
    server.Install_Server_Packet_Handler([&](cPacket &packet, int id) {
        join.Receive(packet, id);
        ++received;
    });

    cConnection nod;
    cConnection gdi;
    const auto pump = [&](const auto &ready) {
        Pump_Clients({&nod, &gdi}, [&] {
            server.Service_Read();
            server.Service_Send(true);
            return ready();
        });
    };
    const auto advance = [&] {
        for (int i = 0; i < 12; ++i) {
            session.Advance(100ms);
            join.Update_Combat();
        }
    };
    Connect_Join_Client(nod, u"First", u"testpass", JoinKey, Receive_First_Join);
    Connect_Join_Client(gdi, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    pump([&] { return FirstJoinClient.OptionsTime && SecondJoinClient.OptionsTime; });
    Check(FirstJoinClient.CanRepairBuildings == can_repair && SecondJoinClient.CanRepairBuildings == can_repair,
        "The original game options did not advertise the building repair setting");
    Send_Bio(nod, u"First", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    Send_Bio(gdi, u"Second", PLAYERTYPE_GDI, 1, "C&C_Test.mix");
    pump([&] { return FirstJoinClient.GameDataTime && SecondJoinClient.GameDataTime; });
    const int thrower = FirstJoinClient.Soldiers.at(1);
    const int engineer = FirstJoinClient.Soldiers.at(2);
    const int repair_player = FirstJoinClient.Players.at(2);
    const int terminal = FirstJoinClient.Buildings.at(301);
    const auto health_code = [](float health) { return static_cast<ULONG>(std::round(health * 2047 / 2000)); };
    const auto controls = [&](cConnection &client, bool create, const Vector3 &aim, ULONG buttons, ULONG trigger) {
        const int peer = client.Get_Local_Id();
        const int before = received;
        cPacket packet;
        Write_Movement(packet, FirstControlObject + (peer - 1) * 100000, create ? peer : 0,
            peer == 1 ? thrower : engineer, aim, 21.0f, 8, buttons, trigger, false);
        client.Send_Packet_To_Individual(packet, SERVER_RHOST_ID, create ? SEND_RELIABLE : SEND_UNRELIABLE);
        pump([&] { return received > before; });
        advance();
    };
    const auto building_health = [&](float expected) {
        return FirstJoinClient.BuildingVitals.at(terminal).Health == health_code(expected) &&
            SecondJoinClient.BuildingVitals.at(terminal).Health == health_code(expected);
    };
    const Vector3 nod_aim(0, -8, 1.62f);
    const Vector3 gdi_aim(-8, -8.5f, 1.62f);
    advance();
    controls(nod, true, nod_aim, 0, 1);
    pump([&] { return building_health(20); });

    controls(gdi, true, gdi_aim, 1u << ControlClass::BOOLEAN_SELECT_WEAPON_4, 0);
    pump([&] { return SecondJoinClient.SelectedWeapon.at(engineer) == 626; });
    controls(gdi, false, gdi_aim, 1u << ControlClass::BOOLEAN_WEAPON_RELOAD, 1);
    pump([&] { return building_health(can_repair ? 40 : 20); });

    controls(nod, false, nod_aim, 1u << ControlClass::BOOLEAN_SELECT_WEAPON_7, 0);
    controls(nod, false, nod_aim, 0, 1);
    pump([&] { return SecondJoinClient.Charges.size() == 1 && SecondJoinClient.Charges.begin()->second.Stuck; });
    const int charge = SecondJoinClient.Charges.begin()->first;
    for (int remaining = 80; remaining >= 20; remaining -= 20) {
        controls(gdi, false, gdi_aim, 0, 1);
        pump([&] {
            return FirstJoinClient.Charges.at(charge).Health == health_code(remaining) &&
                SecondJoinClient.Charges.at(charge).Health == health_code(remaining);
        });
    }
    controls(gdi, false, gdi_aim, 0, 1);
    pump([&] { return FirstJoinClient.Deleted.count(charge) && SecondJoinClient.Deleted.count(charge); });
    for (const auto *client : {&FirstJoinClient, &SecondJoinClient}) {
        Check(client->DisarmOwners == std::vector<int>{thrower} && client->ExplosionOwners.empty() &&
            client->Charges.empty() && client->Vitals.at(engineer).Health == 102 &&
            client->Vitals.at(thrower).Health == 102 && client->KillMessages.empty(),
            "Disarming C4 must publish the harmless effect without combat casualties");
    }
    Check(building_health(can_repair ? 40 : 20), "Disarming C4 changed building health");
    for (int i = 0; i < 3; ++i) {
        controls(gdi, false, gdi_aim, 0, 1);
    }
    const float points = can_repair ? 60.0f : 30.0f;
    pump([&] { return building_health(can_repair ? 80 : 20) && FirstJoinClient.PlayerScores.at(repair_player) == points; });
    Check(SecondJoinClient.PlayerCredits.at(repair_player) == points && SecondJoinClient.Rounds.at(engineer) == -1,
        "Repairs did not respect maximum health, award points, or preserve the unlimited clip");
    controls(nod, false, nod_aim, 1u << ControlClass::BOOLEAN_SELECT_WEAPON_2, 0);
    controls(nod, false, nod_aim, 0, 1);
    controls(nod, false, nod_aim, 0, 1);
    pump([&] { return FirstJoinClient.BuildingDestroyed.at(terminal) && SecondJoinClient.BuildingDestroyed.at(terminal); });
    controls(gdi, false, gdi_aim, 0, 1);
    Check(building_health(0) && SecondJoinClient.PlayerCredits.at(repair_player) == points,
        "The repair beam revived a destroyed building");
}

void Check_Map_Rotation(const HeadlessCollision &collision, const HeadlessCollision &next_collision, bool loop)
{
    FirstJoinClient = {};
    SecondJoinClient = {};
    LateJoinClient = {};
    LoadingJoinClient = {};
    const std::vector<HeadlessDamageRule> rules{{1, 0}};
    const HeadlessJoinData data{42, 1, 2, {{17, rules}, {23, rules}}, {{{400, "soldier0", 100, 50, 1, Fixture_Weapons(0), 600},
        {401, "soldier1", 100, 50, 1, Fixture_Weapons(1), 601}}}};
    auto next_data = data;
    next_data.Soldiers[PLAYERTYPE_NOD].DefinitionId = 410;
    next_data.Soldiers[PLAYERTYPE_GDI].DefinitionId = 411;
    // Test a points lead over more kills when looping, and the kill tiebreaker when ending the cycle.
    const int combat_team = loop ? PLAYERTYPE_NOD : PLAYERTYPE_GDI;
    auto &combat_preset = next_data.Soldiers[combat_team];
    combat_preset.Weapons[1].PrimaryFire->Bullet = HeadlessBulletDefinition{200, 0, 20, 400, 0};
    combat_preset.DamagePoints = loop ? 0.5f : 0.0f;
    combat_preset.DeathPoints = loop ? 4.0f : 0.0f;
    const std::vector<HeadlessMap> maps{{"C&C_Test.mix", collision, data}, {"C&C_Next.mix", next_collision, next_data}};
    HeadlessJoinOptions options;
    options.Owner = u"FixtureHost";
    options.Title = u"Join fixture";
    options.Motd = u"Ready";
    options.Password = u"testpass";
    options.Address = htonl(INADDR_LOOPBACK);
    options.TimeLimit = 23;
    options.LoopMaps = loop;
    options.FriendlyFire = true;
    cConnection server;
    Check(server.Init_As_Server(0, 16, true, INADDR_LOOPBACK), "Could not bind round test server");
    server.Set_Bandwidth_Budget_Out(TestBandwidth);
    JoinPort = server.Get_Local_Port();
    JoinKey = data.VersionKey;
    HeadlessPlayerSession session(maps.front().Collision, server);
    HeadlessJoin join(server, session, maps, options);
    server.Install_Application_Acceptance_Handler([&](cPacket &packet) { return join.Accept(packet); });
    server.Install_Conn_Handler([&](int id) { join.Connected(id); });
    int received = 0;
    server.Install_Server_Packet_Handler([&](cPacket &packet, int id) {
        join.Receive(packet, id);
        ++received;
    });

    cConnection first;
    cConnection second;
    cConnection late;
    cConnection loading;
    const auto pump = [&](const auto &ready) {
        Pump_Clients({&first, &second, &late, &loading}, [&] {
            server.Service_Read();
            join.Close_Pending();
            server.Service_Send(true);
            return ready();
        });
    };
    Connect_Join_Client(first, u"First", u"testpass", JoinKey, Receive_First_Join);
    Connect_Join_Client(second, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    Connect_Join_Client(late, u"Late", u"testpass", JoinKey, Receive_Late_Join);
    Connect_Join_Client(loading, u"Loading", u"testpass", JoinKey, Receive_Loading_Join);
    pump([&] { return FirstJoinClient.OptionsTime && SecondJoinClient.OptionsTime && LateJoinClient.OptionsTime &&
        LoadingJoinClient.OptionsTime; });
    Check(first.Get_Local_Id() == 1 && second.Get_Local_Id() == 2, "Unexpected round test peer IDs");
    cPacket control;
    Write_Movement(control, FirstControlObject, 1, -1, {0, 0, 0});
    first.Send_Packet_To_Individual(control, SERVER_RHOST_ID, SEND_RELIABLE);
    Send_Bio(first, u"First", PLAYERTYPE_GDI, 1, "C&C_Test.mix");
    Send_Bio(second, u"Second", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    pump([&] { return FirstJoinClient.Soldiers.size() == 2 && SecondJoinClient.Soldiers.size() == 2; });
    const auto old_soldiers = FirstJoinClient.Soldiers;
    const auto players = FirstJoinClient.Players;

    const auto expired = HeadlessJoin::Clock::now() + 23min;
    Check(join.Update_Round(expired), "The first map must continue into intermission");
    pump([&] { return FirstJoinClient.Round == 2 && SecondJoinClient.Round == 2; });
    Check(FirstJoinClient.MapCRC == NextMapCRC && SecondJoinClient.MapCRC == NextMapCRC &&
        !FirstJoinClient.CycleEnded && !SecondJoinClient.CycleEnded, "Round result did not name the next map");
    Check(FirstJoinClient.LastWinner == PLAYERTYPE_NOD && SecondJoinClient.LastWinner == PLAYERTYPE_NOD,
        "An exact tie did not use the first team in the original order");
    cPacket announcement;
    join.Write_Lan_Announcement(announcement, options.Address, 1, true);
    Check(Field<BYTE>(announcement) == 0, "Unexpected LAN announcement type");
    WCHAR owner[64];
    announcement.Get_Wide_Terminated_String(owner, 64);
    Check(Field<uint32>(announcement) == 1 && Field<bool>(announcement) && Field<int>(announcement) == 2 &&
        Check_Game_Info(announcement, NextMapCRC) == 4, "LAN discovery must advertise the incoming map");

    const auto finish_loading = [&](cConnection &client, int offset = 2) {
        const int before = received;
        Send_Loading(client, offset, true);
        Send_Loading(client, offset + 1, false);
        pump([&] { return received >= before + 2; });
    };
    join.Update_Round(expired + 9s);
    finish_loading(first);
    Check(FirstJoinClient.Soldiers == old_soldiers && !FirstJoinClient.GameDataTime,
        "A client that loaded early must wait for intermission to end");

    join.Update_Round(expired + 10s);
    pump([&] { return FirstJoinClient.GameDataTime && SecondJoinClient.Soldiers.empty(); });
    Check(*FirstJoinClient.GameDataTime == 1380 && FirstJoinClient.Soldiers.size() == 1 &&
        FirstJoinClient.Soldiers.at(1) != old_soldiers.at(1) && !SecondJoinClient.GameDataTime,
        "Only the ready player should respawn with a reset clock when the next round starts");

    join.Update_Round(expired + 11s);
    finish_loading(second);
    pump([&] { return SecondJoinClient.GameDataTime && FirstJoinClient.Soldiers.size() == 2 &&
        SecondJoinClient.Soldiers.size() == 2; });
    Check(*SecondJoinClient.GameDataTime == 1379 && SecondJoinClient.Soldiers.at(2) != old_soldiers.at(2) &&
        FirstJoinClient.Soldiers == SecondJoinClient.Soldiers && FirstJoinClient.Players == players &&
        SecondJoinClient.Players == players, "Round restart did not preserve the players and replace their soldiers");

    const int new_soldier = FirstJoinClient.Soldiers.at(1);
    Check(std::abs(FirstJoinClient.Y.at(new_soldier) - 1.2f) < 0.1f,
        "The restarted player did not use the next map's spawn position and coordinate precision");
    const int before = received;
    cPacket movement;
    Write_Movement(movement, FirstControlObject, 0, new_soldier, {0, 8, 0}, 61.0f, 10);
    first.Send_Packet_To_Individual(movement, SERVER_RHOST_ID, SEND_UNRELIABLE);
    pump([&] { return received > before; });
    session.Advance(100ms);
    pump([&] { return FirstJoinClient.Y[new_soldier] > 1.5f && SecondJoinClient.Y[new_soldier] > 1.5f; });

    Send_Bio(late, u"Late", combat_team, 1, "C&C_Test.mix");
    pump([&] { return LateJoinClient.Round == 2; });
    Check(LateJoinClient.MapCRC == NextMapCRC && LateJoinClient.Players.count(3) == 1 &&
        LateJoinClient.Soldiers.empty(), "A join finishing on the previous map must be redirected before spawning");
    finish_loading(late);
    pump([&] { return LateJoinClient.GameDataTime && FirstJoinClient.Soldiers.count(3); });

    for (int i = 0; i < 11; ++i) {
        session.Advance(100ms); // Wait out the automatic weapon switch after spawning.
    }
    const int before_shot = received;
    const int shooter = loop ? 2 : 1;
    cPacket friendly_shot;
    if (loop) {
        Write_Movement(friendly_shot, FirstControlObject + 100000, 2, SecondJoinClient.Soldiers.at(2),
            {12, 0, 1.62f}, 61.0f, 10, 0, 1, false);
        second.Send_Packet_To_Individual(friendly_shot, SERVER_RHOST_ID, SEND_RELIABLE);
    } else {
        Write_Movement(friendly_shot, FirstControlObject, 0, new_soldier, {0, -4, 1.62f}, 61.0f, 10, 0, 1, false);
        first.Send_Packet_To_Individual(friendly_shot, SERVER_RHOST_ID, SEND_UNRELIABLE);
    }
    pump([&] { return received > before_shot; });
    session.Advance(50ms);
    join.Update_Combat();
    const float expected_score = loop ? -54 : 0;
    const auto scored = [&](const Join_Client_State &client) {
        const int object = client.Players.at(shooter);
        return Has_Combat_Counts(client, shooter, 3, combat_team, combat_team) &&
            client.PlayerScores.at(object) == expected_score && client.PlayerCredits.at(object) == 0 &&
            client.TeamScores.at(combat_team) == expected_score;
    };
    pump([&] {
        return scored(FirstJoinClient) && scored(SecondJoinClient) && scored(LateJoinClient) &&
            LoadingJoinClient.TeamCounts.at(combat_team) == std::pair<int, int>{1, 1} &&
            LoadingJoinClient.TeamScores.at(combat_team) == expected_score;
    });
    Check(LoadingJoinClient.KillMessages.empty() && LoadingJoinClient.PlayerCounts.empty(),
        "A peer still loading received player statistics or a kill message before the player objects");

    const auto cycle_end = expired + 10s + 23min;
    Check(join.Update_Round(cycle_end), "The last map must send a result before finishing the cycle");
    pump([&] { return FirstJoinClient.Round == 3 && SecondJoinClient.Round == 3 && LateJoinClient.Round == 3; });
    Check(FirstJoinClient.CycleEnded == !loop && FirstJoinClient.MapCRC == (loop ? TestMapCRC : NextMapCRC),
        "The last map did not honor DoMapsLoop");
    Check(FirstJoinClient.LastWinner == PLAYERTYPE_GDI && SecondJoinClient.LastWinner == PLAYERTYPE_GDI &&
        LateJoinClient.LastWinner == PLAYERTYPE_GDI, "The timed result did not rank by score and then kills");
    cPacket final_announcement;
    join.Write_Lan_Announcement(final_announcement, options.Address, 2, true);
    Check(Field<BYTE>(final_announcement) == 0, "Unexpected LAN announcement type after the last map");
    final_announcement.Get_Wide_Terminated_String(owner, 64);
    Check(Field<uint32>(final_announcement) == 2 && Field<bool>(final_announcement) == loop,
        "A finished map cycle must withdraw its LAN entry");
    Check(join.Update_Round(cycle_end + 10s) == loop, "Final intermission must honor DoMapsLoop");
    if (loop) {
        finish_loading(first, 4);
        pump([&] { return FirstJoinClient.GameDataTime && FirstJoinClient.Soldiers.count(1); });
        Check(std::abs(FirstJoinClient.Y.at(FirstJoinClient.Soldiers.at(1)) - 2.0f) < 0.1f,
            "Wrapping the map cycle did not restore the first map's spawn and coordinate precision");
        for (const auto *client : {&FirstJoinClient, &SecondJoinClient, &LateJoinClient, &LoadingJoinClient}) {
            const auto zero = [](const auto &entry) { return entry.second == std::pair<int, int>{0, 0}; };
            const auto zero_points = [](const auto &entry) { return entry.second == 0; };
            Check(std::all_of(client->PlayerCounts.begin(), client->PlayerCounts.end(), zero) &&
                std::all_of(client->TeamCounts.begin(), client->TeamCounts.end(), zero) &&
                std::all_of(client->PlayerScores.begin(), client->PlayerScores.end(), zero_points) &&
                std::all_of(client->PlayerCredits.begin(), client->PlayerCredits.end(), zero_points) &&
                std::all_of(client->TeamScores.begin(), client->TeamScores.end(), zero_points),
                "The next round did not reset player and team combat statistics");
        }
    }
}

void Check_Original_Join(unsigned short port, const std::string &definitions, const std::string &armor,
    LanReceiver &lan)
{
    Check(cNetUtil::Protocol_Init(false), "IPv4 unavailable for the join test");
    FirstJoinClient = {};
    SecondJoinClient = {};
    JoinPort = port;
    const std::string version = "RENEGADE 1234 strings.tdb 145 ";
    auto ddb_crc = CRC::Memory(reinterpret_cast<const unsigned char *>(definitions.data()), definitions.size());
    const auto data_crc = CRC::Memory(reinterpret_cast<const unsigned char *>(armor.data()), armor.size(), ddb_crc);
    JoinKey = static_cast<uint32>(CRCEngine()(version.data(), version.size())) ^ data_crc;
    lan.Expect(true, 0);

    auto refuse = [&](const WCHAR *name, const WCHAR *password, uint32 key, REFUSAL_CODE expected) {
        cConnection client;
        refusal = REFUSAL_CLIENT_ACCEPTED;
        Connect_Join_Client(client, name, password, key, Receive_First_Join);
        Pump_Clients({&client}, [&] { return client.Is_Destroy(); });
        Check(refusal == expected && !client.Is_Established(), "Wrong refusal code for a join request");
    };
    refuse(u"WrongKey", u"testpass", JoinKey ^ 1, REFUSAL_VERSION_MISMATCH);
    refuse(u"WrongPassword", u"wrong", JoinKey, REFUSAL_BAD_PASSWORD);

    cConnection first;
    Connect_Join_Client(first, u"First", u"testpass", JoinKey, Receive_First_Join);
    Pump_Clients({&first}, [&] { return FirstJoinClient.OptionsTime.has_value(); });
    Check(first.Is_Established() && FirstJoinClient.Players.empty(), "Player objects arrived before map load");
    refuse(u"first", u"testpass", JoinKey, REFUSAL_PLAYER_EXISTS);

    cPacket control;
    Write_Movement(control, FirstControlObject, 1, -1, {0, 0, 0});
    first.Send_Packet_To_Individual(control, SERVER_RHOST_ID, SEND_RELIABLE);
    Send_Bio(first, u"First", PLAYERTYPE_GDI, 1, "Wrong.mix");
    const auto deadline = std::chrono::steady_clock::now() + 50ms;
    Pump_Clients({&first}, [&] { return std::chrono::steady_clock::now() >= deadline; });
    Check(FirstJoinClient.Players.empty(), "A bio from a different map spawned a soldier");
    Send_Bio(first, u"First", PLAYERTYPE_GDI, 2, "C&C_Test.mix");
    Pump_Clients({&first}, [&] { return FirstJoinClient.GameDataTime.has_value(); });
    const int first_soldier = FirstJoinClient.Soldiers.at(1);

    const auto late_join = std::chrono::steady_clock::now() + 1100ms;
    Pump_Clients({&first}, [&] { return std::chrono::steady_clock::now() >= late_join; });
    cConnection second;
    Connect_Join_Client(second, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    Pump_Clients({&first, &second}, [&] { return SecondJoinClient.OptionsTime.has_value(); });
    Send_Bio(second, u"Second", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    Pump_Clients({&first, &second}, [&] {
        return SecondJoinClient.GameDataTime && FirstJoinClient.Soldiers.size() == 2 && SecondJoinClient.Soldiers.size() == 2;
    });
    Check(*FirstJoinClient.OptionsTime - *SecondJoinClient.OptionsTime >= 1.0f &&
        *FirstJoinClient.GameDataTime > *SecondJoinClient.GameDataTime,
        "A later player must receive the time remaining in the same round");
    lan.Expect(true, 2);

    cPacket movement;
    Write_Movement(movement, FirstControlObject, 0, first_soldier, {0, 8, 0});
    first.Send_Packet_To_Individual(movement, SERVER_RHOST_ID, SEND_UNRELIABLE);
    Pump_Clients({&first, &second}, [&] {
        return FirstJoinClient.Y[first_soldier] > 2.25f && SecondJoinClient.Y[first_soldier] > 2.25f;
    });

    const auto send_controls = [&](ULONG buttons, ULONG triggers, const Vector3 &target = Vector3(0, 8, 0)) {
        cPacket input;
        Write_Movement(input, FirstControlObject, 0, first_soldier, target, 21.0f, 8, buttons, triggers, false);
        first.Send_Packet_To_Individual(input, SERVER_RHOST_ID, SEND_UNRELIABLE);
    };
    const auto wait = [&](auto elapsed) {
        const auto until = std::chrono::steady_clock::now() + elapsed;
        Pump_Clients({&first, &second}, [&] { return std::chrono::steady_clock::now() >= until; });
    };
    const auto replicated = [&](int rounds, ULONG triggers) {
        return FirstJoinClient.Rounds.at(first_soldier) == rounds &&
            SecondJoinClient.Rounds.at(first_soldier) == rounds &&
            FirstJoinClient.FireControls.at(first_soldier) == triggers &&
            SecondJoinClient.FireControls.at(first_soldier) == triggers;
    };
    const auto fire = [&](ULONG trigger, int remaining, const Vector3 &target = Vector3(0, 8, 0)) {
        send_controls(0, trigger, target);
        Pump_Clients({&first, &second}, [&] { return replicated(remaining, trigger); });
    };
    constexpr ULONG primary = 1;
    constexpr ULONG secondary = 2;
    fire(primary, 220);
    Pump_Clients({&first, &second}, [&] { return replicated(220, 0); });
    wait(300ms); // The first shot's half-second cooldown must finish before reloading.
    send_controls(1u << ControlClass::BOOLEAN_WEAPON_RELOAD, primary);
    Pump_Clients({&first, &second}, [&] { return replicated(220, primary); });
    Pump_Clients({&first, &second}, [&] { return replicated(220, 0); });
    wait(150ms); // The held trigger timed out before the reload could complete.
    fire(secondary, 213);
    wait(550ms);
    fire(primary, 193); // Without the manual reload only 13 of the required 20 rounds would remain in the clip.
    wait(550ms);
    fire(primary, 180); // Empty the clip.
    wait(900ms);
    fire(primary, 160); // The server automatically refilled the clip from the remaining reserve.
    send_controls(0, 0);
    Pump_Clients({&first, &second}, [&] { return replicated(160, 0); });

    const int second_soldier = SecondJoinClient.Soldiers.at(2);
    const auto score = [&](float points) {
        const auto meets = [&](const Join_Client_State &client) {
            const int attacker = client.Players.at(1);
            const int victim = client.Players.at(2);
            return std::abs(client.PlayerScores.at(attacker) - points) < 0.001f &&
                std::abs(client.PlayerCredits.at(attacker) - points) < 0.001f &&
                std::abs(client.TeamScores.at(PLAYERTYPE_GDI) - points) < 0.001f &&
                client.TeamScores.at(PLAYERTYPE_NOD) == 0 &&
                client.PlayerScores.at(victim) == 0 && client.PlayerCredits.at(victim) == 0;
        };
        return meets(FirstJoinClient) && meets(SecondJoinClient);
    };
    const auto defense = [&](int hp, int shield) {
        cEncoderTypeEntry encoder;
        encoder.Init(0, 2000, 1);
        ULONG health_code, shield_code;
        encoder.Scale(hp, health_code);
        encoder.Scale(shield, shield_code);
        const auto matches = [&](const Join_Client_State &client) {
            const auto &vital = client.Vitals.at(second_soldier);
            return vital.Dead == (hp == 0) && vital.Health == health_code && vital.Shield == shield_code;
        };
        return matches(FirstJoinClient) && matches(SecondJoinClient);
    };
    const auto aim_at_second = [&] {
        return Vector3(-12, SecondJoinClient.Y.at(second_soldier) - FirstJoinClient.Y.at(first_soldier), 1.62f);
    };
    wait(550ms);
    fire(primary, 140, aim_at_second());
    wait(900ms);
    Check(defense(100, 50), "Static projectile cover did not block the rifle shot");
    Check(score(0), "A blocked projectile earned points");

    const int second_control_id = FirstControlObject + 100000;
    cPacket sidestep;
    Write_Movement(sidestep, second_control_id, 2, second_soldier, {0, -8, 0}, 21.0f, 8, 0, secondary);
    second.Send_Packet_To_Individual(sidestep, SERVER_RHOST_ID, SEND_RELIABLE);
    wait(300ms);
    Check(SecondJoinClient.Y.at(second_soldier) <= 1.25f, "The target could not leave cover");
    Check(SecondJoinClient.Rounds.at(second_soldier) == 233, "The target did not spend ammunition before dying");
    fire(primary, 120, aim_at_second());
    Check(defense(100, 50), "The rifle projectile arrived before its travel time");
    Pump_Clients({&first, &second}, [&] { return defense(82, 14); });
    Pump_Clients({&first, &second}, [&] { return score(18.0f); });
    wait(550ms);
    fire(primary, 100, aim_at_second());
    Pump_Clients({&first, &second}, [&] { return defense(48, 0); });
    Pump_Clients({&first, &second}, [&] { return score(38.75f); });
    wait(550ms);
    fire(secondary, 93, aim_at_second());
    Pump_Clients({&first, &second}, [&] { return defense(3, 0); });
    Check(FirstJoinClient.KillMessages.empty() && SecondJoinClient.KillMessages.empty() &&
        FirstJoinClient.PlayerCounts.at(FirstJoinClient.Players.at(1)) == std::pair<int, int>{0, 0},
        "A nonlethal hit was counted as a kill");
    wait(550ms);
    fire(secondary, 86, aim_at_second());
    Pump_Clients({&first, &second}, [&] { return defense(0, 0); });
    const auto counted = [&] {
        return Has_Combat_Counts(FirstJoinClient, 1, 2, PLAYERTYPE_GDI, PLAYERTYPE_NOD) &&
            Has_Combat_Counts(SecondJoinClient, 1, 2, PLAYERTYPE_GDI, PLAYERTYPE_NOD);
    };
    Pump_Clients({&first, &second}, counted);
    Pump_Clients({&first, &second}, [&] { return score(66.5f); });

    const float dead_position = SecondJoinClient.Y.at(second_soldier);
    cPacket dead_input;
    Write_Movement(dead_input, second_control_id, 0, second_soldier, {12, 0, 1.62f}, 21.0f, 8,
        1u << ControlClass::BOOLEAN_SELECT_WEAPON_1, primary);
    second.Send_Packet_To_Individual(dead_input, SERVER_RHOST_ID, SEND_UNRELIABLE);
    wait(300ms);
    Check(SecondJoinClient.Y.at(second_soldier) == dead_position &&
        FirstJoinClient.FireControls.at(second_soldier) == 0 && SecondJoinClient.SelectedWeapon.at(second_soldier) == 600,
        "A defeated soldier could still move, change weapons or fire");
    Check(SecondJoinClient.Soldiers.at(2) == second_soldier && !SecondJoinClient.Deleted.count(second_soldier),
        "The server removed the defeated soldier before the respawn delay");

    const auto players = SecondJoinClient.Players;
    const auto round_time = SecondJoinClient.GameDataTime;
    const auto respawned = [&](const Join_Client_State &client) {
        const auto soldier = client.Soldiers.find(2);
        return client.Deleted.count(second_soldier) && soldier != client.Soldiers.end() && soldier->second != second_soldier;
    };
    Pump_Clients({&first, &second}, [&] { return respawned(FirstJoinClient) && respawned(SecondJoinClient); });
    const int replacement = SecondJoinClient.Soldiers.at(2);
    const float spawn_y = SecondJoinClient.Y.at(replacement);
    Check(FirstJoinClient.Soldiers == SecondJoinClient.Soldiers && FirstJoinClient.Players == players &&
        SecondJoinClient.Players == players && SecondJoinClient.Round == 1 && SecondJoinClient.GameDataTime == round_time,
        "Replacing a soldier changed players or reloaded the round");
    Check(std::abs(spawn_y - 2.0f) < 0.1f && SecondJoinClient.Rounds.at(replacement) == 240 &&
        SecondJoinClient.SelectedWeapon.at(replacement) == 600,
        "The replacement did not spawn with the same team's original health, position and loadout");
    Check(counted() && score(66.5f), "Respawning cleared or duplicated combat statistics");

    cPacket stale;
    Write_Movement(stale, second_control_id, 0, second_soldier, {0, -8, 0});
    second.Send_Packet_To_Individual(stale, SERVER_RHOST_ID, SEND_UNRELIABLE);
    wait(300ms);
    Check(SecondJoinClient.Y.at(replacement) == spawn_y, "Old soldier input reached the replacement");
    cPacket resumed;
    Write_Movement(resumed, second_control_id, 0, replacement, {0, -8, 0});
    second.Send_Packet_To_Individual(resumed, SERVER_RHOST_ID, SEND_UNRELIABLE);
    Pump_Clients({&first, &second}, [&] {
        return FirstJoinClient.Y.at(replacement) < spawn_y - 0.4f && SecondJoinClient.Y.at(replacement) < spawn_y - 0.4f;
    });

    const auto select = [&](ControlClass::BooleanControl button, int expected) {
        send_controls(1u << button, 0);
        Pump_Clients({&first, &second}, [&] {
            return FirstJoinClient.SelectedWeapon.at(first_soldier) == expected &&
                SecondJoinClient.SelectedWeapon.at(first_soldier) == expected;
        });
    };
    select(ControlClass::BOOLEAN_WEAPON_NEXT, 620);
    select(ControlClass::BOOLEAN_WEAPON_PREV, 601);
    select(ControlClass::BOOLEAN_SELECT_WEAPON_1, 610);
    select(ControlClass::BOOLEAN_SELECT_NO_WEAPON, 0);
    select(ControlClass::BOOLEAN_WEAPON_NEXT, 610);

    cPacket goodbye;
    goodbye.Add(FirstControlObject + 3);
    goodbye.Add(static_cast<BYTE>(15));
    goodbye.Add(false);
    goodbye.Add(1025); // NETCLASSID_CLIENTGOODBYEEVENT.
    goodbye.Add(1);
    first.Send_Packet_To_Individual(goodbye, SERVER_RHOST_ID, SEND_RELIABLE);
    Pump_Clients({&first, &second}, [&] {
        return SecondJoinClient.Deleted.count(first_soldier) && SecondJoinClient.Deleted.count(FirstJoinClient.Players.at(1));
    });
    lan.Expect(true, 1);
}

} // namespace

int main(int argc, char **argv)
{
    try {
        Check(argc == 2, "Expected the path to renegade_fds");
        TestDirectory fixture;
        const auto data = fixture.Path / "Data";
        fs::create_directory(data);
        const auto settings = data / "Settings.ini";
        const auto master = fixture.Path / "server.ini";

        const auto static_data = Static_Data();
        const auto dynamic_data = Dynamic_Data();
        const auto definitions = Definitions();
        const auto translations = Chunk(0x90000, Chunk(0x07141200, Micro(1, uint32{145}), false));
        const std::string armor = "[Armor_Types]\n2=Flesh\n1=Kevlar\n[Warhead_Types]\n3=None\n2=Bullets\n"
            "[Armor_Save_IDs]\nFlesh=17\nKevlar=23\n[Scale_Flesh]\nBullets=1.5\n"
            "[Scale_Kevlar]\nBullets=2\n[Shield_Kevlar]\nBullets=0.6\n";
        Write_Archive(data / "always.dbs", {{"STRINGS.TDB", translations}, {"OBJECTS.DDB", definitions}});
        std::ofstream(data / "ARMOR.ini") << armor;
        const auto dependencies = Chunk(0x04020527, Micro(1, "D:\\levels\\COLLIDERS.W3D"), false);
        const auto collision_data = Collision_Data();
        Write_Archive(data / "C&C_Test.mix", {{"C&C_TEST.LDD", dynamic_data}, {"C&C_TEST.LSD", static_data},
            {"C&C_TEST.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        std::ofstream(master) << "[Server]\r\nConfig=settings.ini\r\nGameType=LAN\r\nPort=0\r\nNickname=FixtureHost\r\n";
        std::ofstream(settings) << "[settings]\nMapName=c&c_test.MIX ; first map\nMaxPlayers=16\n";

        const MixArchive archive(data / "C&C_Test.mix");
        const MixArchive core(data / "always.dbs");
        const auto level = HeadlessLevel::Load(archive.Find("c&c_test.lsd").value(),
            archive.Find("c&c_test.ldd").value(), core.Find("objects.ddb").value(), std::nullopt);
        Check(level.Static_Objects().size() == 3 && level.Node_Count() == 3 &&
            level.World_Bounds().Extent == Vector3(20, 20, 20) &&
            level.Find_Static_Candidates(Vector3(6, 0, 0)) == std::vector<std::size_t>{0, 1} &&
            level.Find_Static_Candidates(Vector3(-6, 0, 0)) == std::vector<std::size_t>{2} &&
            level.Find_Static_Candidates(Vector3(40, 0, 0)).empty(),
            "Legacy preorder AAB tree links or point query are incorrect");
        Check(level.Static_Objects()[1].InstanceId == 31 && level.Static_Objects()[1].Flags == 15 &&
            level.Static_Objects()[1].AssetName == "TESTMESH" &&
            level.Static_Objects()[1].Transform.Get_Translation() == Vector3(6, 0, 0),
            "Static physics model reference, flags or transform are incorrect");
        Check(level.Count_Player_Spawns(PLAYERTYPE_NOD) == 1 && level.Count_Player_Spawns(PLAYERTYPE_GDI) == 1 &&
            level.Spawners()[0].Transform.Get_Translation() == Vector3(-6, 2, 3),
            "Spawner definitions did not resolve to the correct team and position");
        Check(level.Buildings().size() == 1 && level.Buildings()[0].Id == 42 &&
            level.Buildings()[0].DefinitionId == 300 && level.Buildings()[0].PlayerType == PLAYERTYPE_GDI &&
            level.Buildings()[0].Position == Vector3(5, 1, 2) && !level.Buildings()[0].Powered,
            "Building placement and state did not survive loading");

        const auto collision = HeadlessCollision::Load(level,
            [&](const std::string &name) { return archive.Find(name); }, archive.Find("c&c_test.dep"));
        Check(collision.Collider_Count() == 15 && collision.Incomplete_Object_Count() == 0,
            "HLOD dependencies, top LOD or aggregate parts failed to load");
        const Vector3 start(6.5f, 0.125f, 3), end(6.5f, 0.125f, -1);
        const auto ground = collision.Cast_Ray(start, end, COLLISION_TYPE_PHYSICAL);
        Check(ground && ground->ObjectIndex == 0 && ground->Fraction == 0.75f &&
            ground->Point == Vector3(6.5f, 0.125f, 0) && ground->Normal == Vector3(0, 0, 1) &&
            ground->Surface == SURFACE_TYPE_DIRT && !ground->StartsInside,
            "Physical ray did not return the terrain triangle and its surface");
        const auto cover = collision.Cast_Ray(start, end, COLLISION_TYPE_PROJECTILE);
        Check(cover && std::abs(cover->Point.Z - 1.75f) < 0.0001f && !cover->StartsInside,
            "Projectile ray missed the nearer rotated box");
        const auto inside = collision.Cast_Ray(Vector3(6.5f, 0.125f, 1.5f), end, COLLISION_TYPE_PROJECTILE);
        Check(inside && inside->StartsInside && inside->Fraction == 0,
            "Projectile ray did not report that it started inside the collider");
        const auto camera = collision.Cast_Ray(Vector3(7.5f, 0.125f, 3), Vector3(7.5f, 0.125f, -1),
            COLLISION_TYPE_CAMERA);
        Check(camera && std::abs(camera->Point.Z - 1.75f) < 0.0001f,
            "Aligned aggregate box lost its translated center or world-aligned extents");
        Check(!collision.Cast_Ray(Vector3(7.9f, 2, 3), Vector3(7.9f, 2, -1), COLLISION_TYPE_PHYSICAL),
            "A ray that misses the terrain triangle but passes through its bounds must not hit");
        Check_Box_Sweeps(collision);
        Check_Soldier_Simulation(collision);
        Check_Player_Transport(collision);
        Write_Archive(data / "C&C_Buildings.mix", {{"BUILDINGS.LDD", Dynamic_Data(2, true)},
            {"BUILDINGS.LSD", Static_Data(2, 20, true)}, {"BUILDINGS.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        const MixArchive buildings_archive(data / "C&C_Buildings.mix");
        auto buildings_level = HeadlessLevel::Load(buildings_archive.Find("buildings.lsd").value(),
            buildings_archive.Find("buildings.ldd").value(), core.Find("objects.ddb").value(), std::nullopt);
        const auto buildings_collision = HeadlessCollision::Load(std::move(buildings_level),
            [&](const std::string &name) { return buildings_archive.Find(name); }, buildings_archive.Find("buildings.dep"));
        Check(buildings_collision.Building_For_Object(3) == 0 && buildings_collision.Building_For_Object(4) == 1 &&
            buildings_collision.Level().Static_Objects()[4].MCT, "Building parts were not associated with their owners");
        Check_Building_Combat(buildings_collision, true);
        Check_Building_Combat(buildings_collision, false);
        const HeadlessArmorTable charge_armor{{17, {{0.5f, 0}}}, {23, {{2, 0}}}};
        const auto charge_join_data = Load_Headless_Join_Data(838, charge_armor, core.Find("strings.tdb").value(),
            core.Find("objects.ddb").value(), std::nullopt, [](const std::string &) { return std::nullopt; });
        Write_Archive(data / "C&C_Vehicles.mix", {{"VEHICLES.LDD", Dynamic_Data(2, false, 6, true)},
            {"VEHICLES.LSD", Static_Data()}, {"VEHICLES.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        const MixArchive vehicles_archive(data / "C&C_Vehicles.mix");
        auto vehicles_level = HeadlessLevel::Load(vehicles_archive.Find("vehicles.lsd").value(),
            vehicles_archive.Find("vehicles.ldd").value(), core.Find("objects.ddb").value(), std::nullopt);
        const auto vehicles_collision = HeadlessCollision::Load(std::move(vehicles_level),
            [&](const std::string &name) { return vehicles_archive.Find(name); }, vehicles_archive.Find("vehicles.dep"));
        Check_Vehicle_Seats(vehicles_collision, charge_join_data);
        Check_Building_Combat(buildings_collision, true, &charge_join_data);
        const auto remote_join_data = Load_Headless_Join_Data(838, charge_armor, core.Find("strings.tdb").value(),
            core.Find("objects.ddb").value(), std::nullopt, [](const std::string &) { return std::nullopt; },
            HeadlessSpawnCharacter::Engineer);
        Check_Building_Combat(buildings_collision, true, &remote_join_data, true);
        Write_Archive(data / "C&C_Proximity.mix", {{"PROXIMITY.LDD", Dynamic_Data(2, true, -5.15f)},
            {"PROXIMITY.LSD", Static_Data(2, 20, true)}, {"PROXIMITY.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        const MixArchive proximity_archive(data / "C&C_Proximity.mix");
        auto proximity_level = HeadlessLevel::Load(proximity_archive.Find("proximity.lsd").value(),
            proximity_archive.Find("proximity.ldd").value(), core.Find("objects.ddb").value(), std::nullopt);
        const auto proximity_collision = HeadlessCollision::Load(std::move(proximity_level),
            [&](const std::string &name) { return proximity_archive.Find(name); }, proximity_archive.Find("proximity.dep"));
        const auto proximity_join_data = Load_Headless_Join_Data(838, charge_armor, core.Find("strings.tdb").value(),
            core.Find("objects.ddb").value(), std::nullopt, [](const std::string &) { return std::nullopt; },
            HeadlessSpawnCharacter::Technician);
        Check_Proximity_Combat(proximity_collision, proximity_join_data);
        Write_Archive(data / "repair-fixture.dbs", {{"OBJECTS.DDB", Definitions(true)}});
        const MixArchive repair_archive(data / "repair-fixture.dbs");
        const HeadlessArmorTable repair_armor{{17, {{0.5f, 0}, {1, 0}}}, {23, {{2, 0}, {1, 0}}},
            {25, {{0, 0}, {-1, 0}}}};
        const auto repair_join_data = Load_Headless_Join_Data(838, repair_armor, core.Find("strings.tdb").value(),
            repair_archive.Find("objects.ddb").value(), std::nullopt, [](const std::string &) { return std::nullopt; },
            HeadlessSpawnCharacter::Engineer);
        Check_Repair_Combat(proximity_collision, repair_join_data, true);
        const auto repair_mine_data = Load_Headless_Join_Data(838, repair_armor, core.Find("strings.tdb").value(),
            repair_archive.Find("objects.ddb").value(), std::nullopt, [](const std::string &) { return std::nullopt; },
            HeadlessSpawnCharacter::Technician);
        Check_Repair_Combat(proximity_collision, repair_mine_data, false);
        Write_Archive(data / "C&C_Next.mix", {{"C&C_NEXT.LDD", Dynamic_Data(1.2f)},
            {"C&C_NEXT.LSD", Static_Data(2, 60)}, {"C&C_NEXT.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        const MixArchive next_archive(data / "C&C_Next.mix");
        auto next_level = HeadlessLevel::Load(next_archive.Find("c&c_next.lsd").value(),
            next_archive.Find("c&c_next.ldd").value(), core.Find("objects.ddb").value(), std::nullopt);
        const auto next_collision = HeadlessCollision::Load(std::move(next_level),
            [&](const std::string &name) { return next_archive.Find(name); }, next_archive.Find("c&c_next.dep"));
        Check_Map_Rotation(collision, next_collision, true);
        Check_Map_Rotation(collision, next_collision, false);

        FdsProcess check(argv[1], fixture.Path, master, "--check");
        const auto check_status = check.Wait();
        Check(check_status == 0, "FDS rejected valid fixture: " + check.Output());
        Check(check.Output().find("Validated game data for 1 map(s)") != std::string::npos,
            "FDS did not validate the configured map");
        Check(check.Output().find("3 static objects in 3 spatial nodes, 1 buildings, 0 vehicles, 2 spawners") !=
            std::string::npos,
            "FDS did not load the headless scene before startup");
        Check(check.Output().find("15 colliders, 0 static objects with incomplete geometry; "
            "2/2 soldier-start ground rays hit; 2/2 ground box sweeps hit (0 initial overlaps)") != std::string::npos,
            "FDS did not load W3D collision from the selected map");
        Check(check.Output().find("Headless soldier spawns: Nod ready, GDI ready") != std::string::npos,
            "FDS did not validate grounded spawns for both teams");

        FdsProcess server(argv[1], fixture.Path, master);
        const auto port = server.Wait_For_Port();
        Check_Join_Refused(port);
        FdsProcess occupied(argv[1], fixture.Path, master, "--port", std::to_string(port).c_str());
        Check(occupied.Wait() == 1 && occupied.Output().find("Cannot bind UDP") != std::string::npos,
            "FDS must report an occupied port instead of silently choosing another");
        server.Stop();

        std::ofstream(settings, std::ios::app) << "ClientBuildNumber=1234\nbGameTitle=Join fixture\nbMotd=Ready\n"
            "TimeLimitMinutes=23\nIsPassworded=yes\nbPassword=testpass\n"
            "MapName00=C&C_Test.mix\nMapName01=C&C_Next.mix\nDoMapsLoop=yes\n";
        LanReceiver lan;
        FdsProcess joining(argv[1], fixture.Path, master, "--lan-destination", "127.0.0.1");
        try {
            Check_Original_Join(joining.Wait_For_Port(), definitions, armor, lan);
        } catch (const std::exception &error) {
            throw std::runtime_error(std::string(error.what()) + "\n" + joining.Output());
        }
        joining.Stop();
        lan.Expect(false);

        // The first entry claims an offset outside the archive's data section.
        {
            std::fstream broken(data / "C&C_Test.mix", std::ios::in | std::ios::out | std::ios::binary);
            broken.seekp(16 + static_data.size() + dynamic_data.size() + dependencies.size() + collision_data.size() + 8);
            Write_U32(broken, 0xffffff00u);
        }
        FdsProcess bad_mix(argv[1], fixture.Path, master, "--check");
        Check(bad_mix.Wait() == 1 && bad_mix.Output().find("Invalid MIX1 index entry") != std::string::npos,
            "FDS accepted an invalid map archive");

        Write_Archive(data / "C&C_Test.mix", {{"C&C_TEST.LDD", dynamic_data}, {"C&C_TEST.LSD", static_data},
            {"C&C_TEST.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        Write_Archive(data / "C&C_Next.mix", {{"C&C_NEXT.LDD", Dynamic_Data(1.2f)}, {"C&C_NEXT.LSD", Static_Data(300, 60)}});
        FdsProcess bad_level(argv[1], fixture.Path, master, "--check");
        Check(bad_level.Wait() == 1 && bad_level.Output().find("C&C_Next.mix: ") != std::string::npos &&
            bad_level.Output().find("invalid AAB tree node") != std::string::npos,
            "FDS accepted an invalid static object link on a later map in the cycle");

        Write_Archive(data / "C&C_Test.mix", {{"C&C_TEST.LDD", dynamic_data}, {"C&C_TEST.LSD", static_data},
            {"C&C_TEST.DEP", dependencies}, {"COLLIDERS.W3D", Collision_Data(999)}});
        FdsProcess bad_triangle(argv[1], fixture.Path, master, "--check");
        Check(bad_triangle.Wait() == 1 && bad_triangle.Output().find("Invalid W3D triangle") != std::string::npos,
            "FDS accepted an out-of-range W3D triangle index");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
