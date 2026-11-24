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
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "coltype.h"
#include "connect.h"
#include "crc.h"
#include "combatchunkid.h"
#include "headless_collision.h"
#include "netutil.h"
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

std::string Static_Data(uint32 last_link = 2)
{
    const auto tree = Chunk(1, Bytes(uint32{0x10000}), false) + Tree_Node(0, 20, 3) +
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
        box("legacy.cover", W3D_BOX_ATTRIBUTE_ORIENTED | W3D_MESH_FLAG_COLLISION_TYPE_PROJECTILE,
            {0, 0, 0}, {0.25f, 0.75f, 0.25f}) +
        box("testmesh.camera", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_CAMERA,
            {1, 0, 0}, {0.75f, 0.25f, 0.25f}) +
        box("testmesh.low", W3D_BOX_ATTRIBUTE_ALIGNED | W3D_MESH_FLAG_COLLISION_TYPE_PHYSICAL,
            {0, 0, 0}, {2, 2, 1}) +
        Chunk(W3D_CHUNK_EMITTER, Chunk(W3D_CHUNK_EMITTER_HEADER, Bytes(emitter), false)) +
        Chunk(W3D_CHUNK_HIERARCHY, Chunk(W3D_CHUNK_HIERARCHY_HEADER, Bytes(hierarchy), false) +
            Chunk(W3D_CHUNK_PIVOTS, Bytes(pivots), false)) +
        Chunk(W3D_CHUNK_HMODEL, Chunk(W3D_CHUNK_HMODEL_HEADER, Bytes(hmodel), false) +
            Chunk(W3D_CHUNK_COLLISION_NODE, Bytes(hmodel_node), false)) +
        Chunk(W3D_CHUNK_HLOD, Chunk(W3D_CHUNK_HLOD_HEADER, Bytes(hlod), false) +
            parts(W3D_CHUNK_HLOD_LOD_ARRAY, 1, part("testmesh.low", 0)) +
            parts(W3D_CHUNK_HLOD_LOD_ARRAY, 2, part("testmesh.terrain", 0) + part("legacy", 1)) +
            parts(W3D_CHUNK_HLOD_AGGREGATE_ARRAY, 2, part("testmesh.camera", 1) + part("testmesh.smoke", 1)));
}

std::string Dynamic_Data()
{
    auto spawner = [](sint32 id, sint32 definition, float x) {
        const std::array<float, 12> transform{1, 0, 0, x, 0, 1, 0, 2, 0, 0, 1, 3};
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

std::string Definitions()
{
    auto definition = [](sint32 id, sint32 team) {
        return Chunk(CHUNKID_SPAWNER_DEF, Chunk(0x00100101,
            Chunk(1013991542, Chunk(0x100, Micro(1, id), false)) +
            Chunk(1013991543, Micro(3, team) + Micro(9, uint8{0}) + Micro(15, uint8{1}), false)));
    };
    return Chunk(CHUNKID_SAVELOAD_DEFMGR, Chunk(0x101,
        definition(100, PLAYERTYPE_NOD) + definition(200, PLAYERTYPE_GDI)));
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

        const std::string chunk("\x01\0\0\0\x01\0\0\0\x55", 9);
        const auto static_data = Static_Data();
        const auto dynamic_data = Dynamic_Data();
        Write_Archive(data / "always.dbs", {{"STRINGS.TDB", chunk}, {"OBJECTS.DDB", Definitions()}});
        std::ofstream(data / "ARMOR.ini") << "[Armor]\n";
        const auto dependencies = Chunk(0x04020527, Micro(1, "D:\\levels\\COLLIDERS.W3D"), false);
        const auto collision_data = Collision_Data();
        Write_Archive(data / "C&C_Test.mix", {{"C&C_TEST.LDD", dynamic_data}, {"C&C_TEST.LSD", static_data},
            {"C&C_TEST.DEP", dependencies}, {"COLLIDERS.W3D", collision_data}});
        std::ofstream(master) << "[Server]\r\nConfig=settings.ini\r\nGameType=LAN\r\nPort=0\r\n";
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
        Check(collision.Collider_Count() == 9 && collision.Incomplete_Object_Count() == 0,
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

        FdsProcess check(argv[1], fixture.Path, master, "--check");
        const auto check_status = check.Wait();
        Check(check_status == 0, "FDS rejected valid fixture: " + check.Output());
        Check(check.Output().find("Validated game data for 1 map(s)") != std::string::npos,
            "FDS did not validate the configured map");
        Check(check.Output().find("3 static objects in 3 spatial nodes, 1 buildings, 2 spawners") != std::string::npos,
            "FDS did not load the headless scene before startup");
        Check(check.Output().find("9 colliders, 0 static objects with incomplete geometry; "
            "2/2 soldier-start ground rays hit") != std::string::npos,
            "FDS did not load W3D collision from the selected map");

        FdsProcess server(argv[1], fixture.Path, master);
        const auto port = server.Wait_For_Port();
        Check_Join_Refused(port);
        FdsProcess occupied(argv[1], fixture.Path, master, "--port", std::to_string(port).c_str());
        Check(occupied.Wait() == 1 && occupied.Output().find("Cannot bind UDP") != std::string::npos,
            "FDS must report an occupied port instead of silently choosing another");
        server.Stop();

        // The first entry claims an offset outside the archive's data section.
        {
            std::fstream broken(data / "C&C_Test.mix", std::ios::in | std::ios::out | std::ios::binary);
            broken.seekp(16 + static_data.size() + dynamic_data.size() + dependencies.size() + collision_data.size() + 8);
            Write_U32(broken, 0xffffff00u);
        }
        FdsProcess bad_mix(argv[1], fixture.Path, master, "--check");
        Check(bad_mix.Wait() == 1 && bad_mix.Output().find("Invalid MIX1 index entry") != std::string::npos,
            "FDS accepted an invalid map archive");

        Write_Archive(data / "C&C_Test.mix", {{"C&C_TEST.LDD", dynamic_data}, {"C&C_TEST.LSD", Static_Data(300)}});
        FdsProcess bad_level(argv[1], fixture.Path, master, "--check");
        Check(bad_level.Wait() == 1 && bad_level.Output().find("invalid AAB tree node") != std::string::npos,
            "FDS accepted an out-of-range static object link in an otherwise valid archive");

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
