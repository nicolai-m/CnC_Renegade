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

std::string Static_Data(uint32 last_link = 2, float world_extent = 20)
{
    const auto tree = Chunk(1, Bytes(uint32{0x10000}), false) + Tree_Node(0, world_extent, 3) +
        Tree_Node(6, 3, 0) + Tree_Node(-6, 3, 0);
    auto linked_object = [](uint32 node, uint32 id, float x) {
        const std::array<float, 12> transform{1, 0, 0, x, 0, 1, 0, 0, 0, 0, 1, 0};
        const auto vars = Chunk(0x660055, Micro(3, uint32{15}) + Micro(7, id), false);
        const auto model = Chunk(0x660056, Chunk(CHUNKID_WW3D_BEGIN,
            Chunk(0x555040, Micro(1, "TESTMESH") + Micro(2, transform), false)));
        return Chunk(0x770101, Chunk(0x20002, Chunk(0x00100101, vars + model))) +
            Chunk(0x770102, Chunk(0x200, Bytes(node), false));
    };
    return Chunk(CHUNKID_WWPHYS_BEGIN, Chunk(0x04433220, Chunk(0x4500, Chunk(0x104, tree)))) +
        Chunk(CHUNKID_WWPHYS_BEGIN + 1, Chunk(0x06090609, Chunk(0x770100,
            linked_object(1, 30, 6) + linked_object(1, 31, 6) + linked_object(last_link, 32, -6))));
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
        Chunk(W3D_CHUNK_EMITTER, Chunk(W3D_CHUNK_EMITTER_HEADER, Bytes(emitter), false)) +
        Chunk(W3D_CHUNK_HIERARCHY, Chunk(W3D_CHUNK_HIERARCHY_HEADER, Bytes(hierarchy), false) +
            Chunk(W3D_CHUNK_PIVOTS, Bytes(pivots), false)) +
        Chunk(W3D_CHUNK_HMODEL, Chunk(W3D_CHUNK_HMODEL_HEADER, Bytes(hmodel), false) +
            Chunk(W3D_CHUNK_COLLISION_NODE, Bytes(hmodel_node), false)) +
        Chunk(W3D_CHUNK_HLOD, Chunk(W3D_CHUNK_HLOD_HEADER, Bytes(hlod), false) +
            parts(W3D_CHUNK_HLOD_LOD_ARRAY, 1, part("testmesh.low", 0)) +
            parts(W3D_CHUNK_HLOD_LOD_ARRAY, 2, part("testmesh.terrain", 0) + part("legacy", 1)) +
            parts(W3D_CHUNK_HLOD_AGGREGATE_ARRAY, 3, part("testmesh.camera", 1) + part("testmesh.smoke", 1) +
                part("testmesh.wall", 0)));
}

std::string Dynamic_Data(float spawn_y = 2)
{
    auto spawner = [spawn_y](sint32 id, sint32 definition, float x) {
        const std::array<float, 12> transform{1, 0, 0, x, 0, 1, 0, spawn_y, 0, 0, 1, 3};
        return Chunk(1014991133, Chunk(1014991054,
            Micro(1, id) + Micro(2, transform) + Micro(3, definition) + Micro(6, uint8{1}), false));
    };
    const auto spawners = Chunk(916991657, spawner(10, 100, -6) + spawner(20, 200, 6));
    const auto building = Chunk(CHUNKID_GAME_OBJECT_POWERPLANT, Chunk(0x00100101,
        Chunk(207011121, Micro(1, std::array<float, 3>{5, 1, 2}) + Micro(2, uint8{0}), false) +
        Chunk(207011120, Chunk(207011214, Micro(1, sint32{PLAYERTYPE_GDI}), false) +
            Chunk(910991407, Micro(2, sint32{300}) + Micro(3, sint32{42}), false))));
    const auto objects = Chunk(916991654, Chunk(916991653, building));
    return Chunk(1011991649, Chunk(CHUNKID_COMBAT, objects + spawners));
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

std::string Definitions()
{
    auto definition = [](sint32 id, sint32 team) {
        return Chunk(CHUNKID_SPAWNER_DEF, Chunk(0x00100101,
            Chunk(1013991542, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1013991543, Micro(3, team) + Micro(9, uint8{0}) + Micro(15, uint8{1}), false)));
    };
    auto soldier = [](sint32 id, const char (&name)[21], sint32 physics) {
        const auto identity = Chunk(0x100, Micro(1, id) + Micro(3, name), false);
        const auto defense = Chunk(7311607, Micro(0, 100.0f) + Micro(3, 50.0f) + Micro(5, uint32{1}), false);
        const auto physical = Chunk(418001829, Chunk(909991657, Micro(18, physics), false));
        const auto weapons = Chunk(418001830, Micro(11, sint32{600 + id - 400}) + Micro(12, sint32{500}) +
            Micro(14, sint32{620}), false);
        const auto scripts = Chunk(627001057, Micro(2, "M00_GrantPowerup_Created") + Micro(3, "POW_Pistol"), false);
        return Chunk(CHUNKID_GAME_OBJECT_DEF_SOLDIER,
            Chunk(0x00100101, identity + defense + physical + weapons + scripts));
    };
    auto physics = [](sint32 id, const char (&model)[18]) {
        return Chunk(0x20501, Chunk(0x00100101, Chunk(0x100, Micro(1, id), false) +
            Chunk(0x055ffe08, Micro(1, model), false)));
    };
    auto weapon = [](sint32 id, sint32 clip, sint32 reserve, float key, sint32 style) {
        return Chunk(CHUNKID_WEAPON_DEF, Chunk(0x00100101,
            Chunk(1205091655, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1205091654, Micro(1, style) + Micro(9, 0.35f) + Micro(27, sint32{640}) +
                Micro(28, sint32{641}) + Micro(29, clip) + Micro(35, reserve) + Micro(37, key), false)));
    };
    auto ammo = [](sint32 id, sint32 cost) {
        return Chunk(CHUNKID_AMMO_DEF, Chunk(0x00100101,
            Chunk(1206091430, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1206091429, Micro(31, cost) + Micro(48, 2.0f), false)));
    };
    const auto powerup = Chunk(CHUNKID_GAME_OBJECT_DEF_POWERUP, Chunk(0x00100101,
        Chunk(909991656, Chunk(0x100, Micro(1, sint32{630}) + Micro(3, "pow_pistol"), false)) +
        Chunk(909991657, Micro(8, sint32{610}) + Micro(9, uint8{1}) + Micro(10, sint32{-1}), false)));
    return Chunk(CHUNKID_SAVELOAD_DEFMGR, Chunk(0x101,
        definition(100, PLAYERTYPE_NOD) + definition(200, PLAYERTYPE_GDI) +
        soldier(400, "CnC_Nod_Minigunner_0", 500) + soldier(401, "CnC_GDI_MiniGunner_0", 501) +
        physics(500, "test\\soldier0.w3d") + physics(501, "test\\soldier1.w3d") +
        weapon(600, 40, 200, 2, 2) + weapon(601, 40, 200, 2, 2) + weapon(610, 12, 100, 1, 5) +
        weapon(620, 1, 0, 7, 0) + ammo(640, 20) + ammo(641, 7) + powerup));
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

void Write_Movement(cPacket &packet, int control_object, int owner, int soldier, float target_y,
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
    for (const float coordinate : {0.0f, target_y, 0.0f}) {
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
    Write_Movement(wrong_owner, FirstControlObject, 2, SoldierObject, 8);
    send(wrong_owner, SEND_RELIABLE);
    cPacket unregistered;
    Write_Movement(unregistered, FirstControlObject, 0, SoldierObject, 8);
    send(unregistered, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position == spawn, "An invalid control owner moved the soldier");

    cPacket creation;
    Write_Movement(creation, FirstControlObject, 1, -1, 0);
    send(creation, SEND_RELIABLE);
    cPacket wrong_soldier;
    Write_Movement(wrong_soldier, FirstControlObject, 0, SoldierObject + 1, 8);
    send(wrong_soldier, SEND_UNRELIABLE);
    cPacket truncated;
    Write_Movement(truncated, FirstControlObject, 0, SoldierObject, 8);
    truncated.Set_Bit_Length(truncated.Get_Bit_Length() - 1);
    send(truncated, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position == spawn, "Wrong-target or truncated input moved the soldier");

    cPacket drive;
    Write_Movement(drive, FirstControlObject, 0, SoldierObject, 8);
    send(drive, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position.Y > spawn.Y + 0.2f, "Valid client movement did not reach the simulation");
    const auto wall = snapshot(100ms).Position;
    Check(std::abs(wall.Y - 2.448f) < 0.1f, "Networked soldier did not stop at the wall");

    cPacket retreat;
    Write_Movement(retreat, FirstControlObject, 0, SoldierObject, -8);
    send(retreat, SEND_UNRELIABLE);
    Check(snapshot(100ms).Position.Y < wall.Y - 0.2f, "Turning the aim did not change the movement direction");
    const auto before_timeout = snapshot(120ms).Position;
    Check(snapshot(100ms).Position == before_timeout, "Expired movement input continued driving the soldier");
    session.Remove_Player(client.Get_Local_Id());
    Check(session.Attach_Player(1, PLAYERTYPE_GDI, SoldierObject, {}) && snapshot(50ms).Position == spawn,
        "Reattaching a peer retained the previous soldier's position or input");

    cPacket recreate;
    Write_Movement(recreate, FirstControlObject + 1, 1, SoldierObject, -8);
    send(recreate, SEND_RELIABLE);
    snapshot(100ms);
    MovementSnapshot edge;
    for (int i = 0; i < 7; ++i) {
        cPacket walk;
        Write_Movement(walk, FirstControlObject + 1, 0, SoldierObject, -8);
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
    std::set<int> Teams;
    std::map<int, int> Players; // Peer ID to network object ID.
    std::map<int, int> Soldiers;
    std::map<int, float> Y;
    std::map<int, int> SelectedWeapon;
    std::map<int, int> Rounds;
    std::map<int, ULONG> FireControls;
    std::set<int> Deleted;
    std::optional<float> OptionsTime;
    std::optional<int> GameDataTime;
    int Round = 1;
    uint32 MapCRC = 0;
    bool CycleEnded = false;
};

Join_Client_State FirstJoinClient;
Join_Client_State SecondJoinClient;
Join_Client_State LateJoinClient;
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
    for (int i = 0; i < 4; ++i) {
        Check(Field<uint32>(packet) == 0, "Unexpected player statistics");
    }
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

std::vector<HeadlessSpawnWeapon> Fixture_Weapons(int team)
{
    const HeadlessFireMode primary{0.5f, 0, 20};
    const HeadlessFireMode secondary{0.5f, 0, 7};
    return {{610, 1, 12, 12, -1, 0.35f, primary, secondary},
        {600 + team, 2, 40, 40, 200, 0.35f, primary, secondary},
        {620, 7, 1, 1, 0, 0.35f, {}, {}}};
}

void Read_Soldier_Frequent(cPacket &packet, Join_Client_State &client, int object)
{
    Check(!Field<bool>(packet), "Soldier was unexpectedly in a vehicle");
    int id = 0;
    int rounds = 0;
    if (Field<bool>(packet)) {
        id = Field<int>(packet);
        rounds = Field<int>(packet);
        Check((id == 610 && rounds == -1) || ((id == 600 || id == 601) && rounds >= 0 && rounds <= 240) ||
            (id == 620 && rounds == 1), "Incorrect equipped weapon or ammo count");
    }
    client.SelectedWeapon[object] = id;
    client.Rounds[object] = rounds;
    client.Y[object] = Read_Join_Position_Y(packet, client.MapCRC);
    ULONG state, bits;
    packet.Get_Bits(state, 5);
    packet.Get_Bits(bits, 9); // Human substate.
    Check(state == 0 || state == 5, "Unexpected soldier state");
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

void Receive_Join_Packet(cPacket &packet, Join_Client_State &client)
{
    const int object = Field<int>(packet);
    const auto dirty = Field<BYTE>(packet);
    const bool deleting = Field<bool>(packet);
    if (dirty == 0 && deleting) {
        client.Deleted.insert(object);
        const auto old_soldier = std::find_if(client.Soldiers.begin(), client.Soldiers.end(), [object](const auto &entry) {
            return entry.second == object;
        });
        if (old_soldier != client.Soldiers.end()) {
            client.Soldiers.erase(old_soldier);
        }
        Check(packet.Is_Flushed(), "Unexpected data in object deletion");
        return;
    }
    if (dirty == 1) {
        Check(!deleting, "Unexpected deletion flag on soldier update");
        Read_Soldier_Frequent(packet, client, object);
        Check(packet.Is_Flushed(), "Unexpected data in soldier update");
        return;
    }
    if (dirty == NetworkObjectClass::BIT_RARE) { // cPlayer update after loading.
        Read_Player(packet, client, object, false);
        Check(!deleting && packet.Is_Flushed(), "Incorrect player update after loading");
        return;
    }
    Check(dirty == 15, "Unexpected object update in the join sequence");
    switch (Field<int>(packet)) {
    case 1010: // cTeam.
        Check(!deleting && client.Teams.insert(Field<int>(packet)).second &&
            Field<int>(packet) == 0 && Field<int>(packet) == 0 && Field<float>(packet) == 0,
            "Incorrect team creation");
        break;
    case 1008: { // cGameOptionsEvent.
        Check(client.Teams == std::set<int>{PLAYERTYPE_NOD, PLAYERTYPE_GDI}, "Teams must arrive before game options");
        Check_Game_Info(packet);
        Check(Field<int>(packet) == 23 && Field<int>(packet) == 1 &&
            Field<int>(packet) == 10 && Field<int>(packet) == 0, "Wrong second-tier game settings");
        ULONG flags;
        packet.Get_Bits(flags, 7);
        WCHAR motd[100];
        packet.Get_Wide_Terminated_String(motd, 100, true);
        Check(std::u16string(motd) == u"Ready", "Wrong message of the day in game options");
        Check(!Field<bool>(packet) && !Field<bool>(packet) && Field<int>(packet) == 0,
            "Incorrect C&C victory options or starting credits");
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
    case 1000: { // Soldier GameObj with factory data and all update tiers.
        const int preset = Field<int>(packet);
        const int first_preset = client.MapCRC == NextMapCRC ? 410 : 400;
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
            !Field<bool>(packet) && Field<int>(packet) == preset && !Field<bool>(packet),
            "Incorrect soldier rare fields");
        ULONG defense;
        packet.Get_Bits(defense, 11);
        Check(defense == 102, "Wrong initial health");
        packet.Get_Bits(defense, 11);
        Check(defense == 51, "Wrong initial shield");
        packet.Get_Bits(defense, 2);
        const auto inventory = Fixture_Weapons(preset - first_preset);
        Check(defense == 2 && Field<int>(packet) == static_cast<int>(inventory.size()),
            "Wrong shield type or size of the spawn inventory");
        for (const auto &weapon : inventory) {
            Check(Field<int>(packet) == weapon.DefinitionId && Field<int>(packet) == weapon.Total_Rounds(),
                "Spawn inventory did not include the armed preset and the scripted weapon grant");
        }
        Read_Soldier_Frequent(packet, client, object);
        Check(client.SelectedWeapon.at(object) == 600 + preset - first_preset, "The primary weapon must start equipped");
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
    case 1003: { // cWinEvent.
        Check(deleting && Field<int>(packet) == PLAYERTYPE_NOD && Field<int>(packet) == -99999 &&
            Field<int>(packet) == client.Round, "Incorrect timed round result");
        client.CycleEnded = Field<bool>(packet);
        Check(Field<int>(packet) == 2 && Field<uint32>(packet) == 1380, "Incorrect reason or duration for round end");
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

void Check_Map_Rotation(const HeadlessCollision &collision, const HeadlessCollision &next_collision, bool loop)
{
    FirstJoinClient = {};
    SecondJoinClient = {};
    LateJoinClient = {};
    const HeadlessJoinData data{42, 1, 2, 2, {{{400, "soldier0", 100, 50, 1, Fixture_Weapons(0), 600},
        {401, "soldier1", 100, 50, 1, Fixture_Weapons(1), 601}}}};
    auto next_data = data;
    next_data.Soldiers[PLAYERTYPE_NOD].DefinitionId = 410;
    next_data.Soldiers[PLAYERTYPE_GDI].DefinitionId = 411;
    const std::vector<HeadlessMap> maps{{"C&C_Test.mix", collision, data}, {"C&C_Next.mix", next_collision, next_data}};
    HeadlessJoinOptions options;
    options.Owner = u"FixtureHost";
    options.Title = u"Join fixture";
    options.Motd = u"Ready";
    options.Password = u"testpass";
    options.Address = htonl(INADDR_LOOPBACK);
    options.TimeLimit = 23;
    options.LoopMaps = loop;
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
    const auto pump = [&](const auto &ready) {
        Pump_Clients({&first, &second, &late}, [&] {
            server.Service_Read();
            join.Close_Pending();
            server.Service_Send(true);
            return ready();
        });
    };
    Connect_Join_Client(first, u"First", u"testpass", JoinKey, Receive_First_Join);
    Connect_Join_Client(second, u"Second", u"testpass", JoinKey, Receive_Second_Join);
    Connect_Join_Client(late, u"Late", u"testpass", JoinKey, Receive_Late_Join);
    pump([&] { return FirstJoinClient.OptionsTime && SecondJoinClient.OptionsTime && LateJoinClient.OptionsTime; });
    Check(first.Get_Local_Id() == 1 && second.Get_Local_Id() == 2, "Unexpected round test peer IDs");
    cPacket control;
    Write_Movement(control, FirstControlObject, 1, -1, 0);
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
    cPacket announcement;
    join.Write_Lan_Announcement(announcement, options.Address, 1, true);
    Check(Field<BYTE>(announcement) == 0, "Unexpected LAN announcement type");
    WCHAR owner[64];
    announcement.Get_Wide_Terminated_String(owner, 64);
    Check(Field<uint32>(announcement) == 1 && Field<bool>(announcement) && Field<int>(announcement) == 2 &&
        Check_Game_Info(announcement, NextMapCRC) == 3, "LAN discovery must advertise the incoming map");

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
    Write_Movement(movement, FirstControlObject, 0, new_soldier, 8, 61.0f, 10);
    first.Send_Packet_To_Individual(movement, SERVER_RHOST_ID, SEND_UNRELIABLE);
    pump([&] { return received > before; });
    session.Advance(100ms);
    pump([&] { return FirstJoinClient.Y[new_soldier] > 1.5f && SecondJoinClient.Y[new_soldier] > 1.5f; });

    Send_Bio(late, u"Late", PLAYERTYPE_NOD, 1, "C&C_Test.mix");
    pump([&] { return LateJoinClient.Round == 2; });
    Check(LateJoinClient.MapCRC == NextMapCRC && LateJoinClient.Players.count(3) == 1 &&
        LateJoinClient.Soldiers.empty(), "A join finishing on the previous map must be redirected before spawning");
    finish_loading(late);
    pump([&] { return LateJoinClient.GameDataTime && FirstJoinClient.Soldiers.count(3); });

    const auto cycle_end = expired + 10s + 23min;
    Check(join.Update_Round(cycle_end), "The last map must send a result before finishing the cycle");
    pump([&] { return FirstJoinClient.Round == 3 && SecondJoinClient.Round == 3 && LateJoinClient.Round == 3; });
    Check(FirstJoinClient.CycleEnded == !loop && FirstJoinClient.MapCRC == (loop ? TestMapCRC : NextMapCRC),
        "The last map did not honor DoMapsLoop");
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
    Write_Movement(control, FirstControlObject, 1, -1, 0);
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
    Write_Movement(movement, FirstControlObject, 0, first_soldier, 8);
    first.Send_Packet_To_Individual(movement, SERVER_RHOST_ID, SEND_UNRELIABLE);
    Pump_Clients({&first, &second}, [&] {
        return FirstJoinClient.Y[first_soldier] > 2.25f && SecondJoinClient.Y[first_soldier] > 2.25f;
    });

    const auto send_controls = [&](ULONG buttons, ULONG triggers) {
        cPacket input;
        Write_Movement(input, FirstControlObject, 0, first_soldier, 8, 21.0f, 8, buttons, triggers, false);
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
    const auto fire = [&](ULONG trigger, int remaining) {
        send_controls(0, trigger);
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
        const std::string armor = "[Armor_Types]\n0=None\n1=Soldier\n";
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
        Check(collision.Collider_Count() == 12 && collision.Incomplete_Object_Count() == 0,
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
        Check(check.Output().find("3 static objects in 3 spatial nodes, 1 buildings, 2 spawners") != std::string::npos,
            "FDS did not load the headless scene before startup");
        Check(check.Output().find("12 colliders, 0 static objects with incomplete geometry; "
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
