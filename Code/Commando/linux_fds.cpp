// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aabox.h"
#include "chunkio.h"
#include "coltype.h"
#include "connect.h"
#include "headless_collision.h"
#include "headless_join.h"
#include "headless_player_session.h"
#include "headless_simulation.h"
#include "mixarchive.h"
#include "netutil.h"
#include "packetmgr.h"
#include "playertype.h"

namespace {

namespace fs = std::filesystem;

struct Options {
    fs::path Config = "server.ini";
    fs::path DataDir = ".";
    std::string BindAddress = "127.0.0.1";
    std::optional<unsigned short> Port;
    std::optional<in_addr> LanDestination;
    bool CheckOnly = false;
    bool Help = false;
};

std::string Fold_Ascii(std::string_view value)
{
    std::string result(value);
    for (char &c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

std::string_view Trim(std::string_view value)
{
    constexpr std::string_view spaces = " \t\r\n\v\f";
    const auto first = value.find_first_not_of(spaces);
    return first == std::string_view::npos ? std::string_view{} :
        value.substr(first, value.find_last_not_of(spaces) - first + 1);
}

int Read_Integer(std::string_view value, int minimum, int maximum, const char *field)
{
    int number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        number < minimum || number > maximum) {
        throw std::runtime_error(std::string(field) + " must be an integer between " +
            std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return number;
}

unsigned short Read_Port(std::string_view value, const char *field)
{
    if (value == "0") {
        return 0; // Ask the OS for a free port.
    }
    return static_cast<unsigned short>(Read_Integer(value, MIN_SERVER_PORT, MAX_SERVER_PORT, field));
}

Options Parse_Options(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string_view option = argv[i];
        if (option == "--help" || option == "-h") {
            options.Help = true;
            return options;
        }
        if (option == "--check") {
            options.CheckOnly = true;
            continue;
        }
        if (option != "--config" && option != "--data-dir" && option != "--bind" && option != "--port" &&
            option != "--lan-destination") {
            throw std::runtime_error("Unknown option: " + std::string(option));
        }
        if (++i == argc || std::string_view(argv[i]).empty() || std::string_view(argv[i]).substr(0, 2) == "--") {
            throw std::runtime_error("Missing value for " + std::string(option));
        }
        if (option == "--config") {
            options.Config = argv[i];
        } else if (option == "--data-dir") {
            options.DataDir = argv[i];
        } else if (option == "--bind") {
            options.BindAddress = argv[i];
        } else if (option == "--port") {
            options.Port = Read_Port(argv[i], "--port");
        } else {
            in_addr destination{};
            if (inet_pton(AF_INET, argv[i], &destination) != 1 || destination.s_addr == INADDR_ANY ||
                IN_MULTICAST(ntohl(destination.s_addr))) {
                throw std::runtime_error("--lan-destination must be a broadcast or unicast IPv4 address");
            }
            options.LanDestination = destination;
        }
    }
    return options;
}

using IniSection = std::map<std::string, std::string>;
using IniFile = std::map<std::string, IniSection>;
using IniEntryOrder = std::map<std::string, std::vector<std::string>>;

IniFile Read_Ini(std::istream &input, const std::string &name, IniEntryOrder *order = nullptr)
{
    IniFile result;
    IniSection *section = nullptr;
    std::vector<std::string> *keys = nullptr;
    std::string line;
    bool first_line = true;
    while (std::getline(input, line)) {
        std::string_view text = line;
        if (first_line && text.substr(0, 3) == "\xef\xbb\xbf") {
            text.remove_prefix(3);
        }
        first_line = false;
        text = Trim(text.substr(0, text.find(';')));
        if (text.empty()) {
            continue;
        }
        if (text.front() == '[' && text.back() == ']') {
            const auto name = Fold_Ascii(Trim(text.substr(1, text.size() - 2)));
            section = &result[name];
            keys = order ? &(*order)[name] : nullptr;
            continue;
        }
        const auto equals = text.find('=');
        if (section != nullptr && equals != std::string_view::npos) {
            const auto key = Trim(text.substr(0, equals));
            if (!key.empty()) {
                const auto folded = Fold_Ascii(key);
                if (keys && section->find(folded) == section->end()) {
                    keys->push_back(folded);
                }
                (*section)[folded] = Trim(text.substr(equals + 1));
            }
        }
    }
    if (input.bad()) {
        throw std::runtime_error("Error reading configuration: " + name);
    }
    return result;
}

IniFile Read_Ini(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read configuration: " + path.string());
    }
    return Read_Ini(input, path.string());
}

std::string Setting(const IniSection &section, const std::string &key)
{
    const auto found = section.find(key);
    return found == section.end() ? std::string{} : found->second;
}

std::optional<fs::path> Find_Child(const fs::path &directory, std::string_view name)
{
    constexpr std::string_view invalid("/\\:\0", 4);
    if (name.empty() || name == "." || name == ".." || name.find_first_of(invalid) != std::string_view::npos) {
        throw std::runtime_error("Expected a file or directory name without a path");
    }
    const auto exact = directory / name;
    if (fs::exists(exact)) {
        return exact;
    }
    const auto folded = Fold_Ascii(name);
    std::optional<fs::path> match;
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (Fold_Ascii(entry.path().filename().string()) == folded) {
            if (match) {
                throw std::runtime_error("Ambiguous filename in " + directory.string() + ": " + std::string(name));
            }
            match = entry.path();
        }
    }
    return match;
}

fs::path Resolve_Data_Directory(const fs::path &root)
{
    if (!fs::is_directory(root)) {
        throw std::runtime_error("Game data directory does not exist: " + root.string());
    }
    const auto nested = Find_Child(root, "data");
    return nested && fs::is_directory(*nested) ? *nested : root;
}

struct Settings {
    std::vector<std::string> Maps;
    int MaxPlayers = 16;
    unsigned short Port = 4848;
    std::optional<uint32> ClientBuild;
    HeadlessJoinOptions Join;
};

std::u16string Legacy_Ini_String(const std::string &value, std::size_t limit, const char *field)
{
    if (value.size() > limit || value.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string("Invalid ") + field + " length");
    }
    std::u16string result;
    for (const unsigned char c : value) {
        result.push_back(c);
    }
    return result;
}

Settings Load_Settings(const Options &options, const fs::path &data_dir)
{
    auto ini = Read_Ini(options.Config);
    std::string port;
    std::string nickname;
    const auto server = ini.find("server");
    if (server != ini.end()) {
        const auto game_type = Fold_Ascii(Setting(server->second, "gametype"));
        if (!game_type.empty() && game_type != "lan") {
            throw std::runtime_error("Only GameType=LAN is supported by the Linux bootstrap");
        }
        port = Setting(server->second, "port");
        nickname = Setting(server->second, "nickname");
        const auto config = Setting(server->second, "config");
        if (!config.empty()) {
            const auto path = Find_Child(data_dir, config);
            if (!path) {
                throw std::runtime_error("[Server] Config was not found in the data directory: " + config);
            }
            ini = Read_Ini(*path);
        }
    }

    const auto rules = ini.find("settings");
    if (rules == ini.end()) {
        throw std::runtime_error("Expected [Settings] or [Server] Config pointing to a game settings INI");
    }
    if (!Setting(rules->second, "modname").empty()) {
        throw std::runtime_error("ModName requires the mod package loader, which is still being ported");
    }

    Settings result;
    if (const auto count = Setting(rules->second, "maxplayers"); !count.empty()) {
        result.MaxPlayers = Read_Integer(count, 1, cConnection::MAX_PEER_ID, "MaxPlayers");
    }
    if (port.empty()) {
        port = Setting(rules->second, "port");
    }
    if (options.Port) {
        result.Port = *options.Port;
    } else if (!port.empty()) {
        result.Port = Read_Port(port, "Port");
    }
    const auto start = Setting(rules->second, "mapname");
    if (!start.empty()) {
        result.Maps.push_back(start);
    }
    for (int i = 0; i < 100; ++i) {
        const auto key = std::string(i < 10 ? "mapname0" : "mapname") + std::to_string(i);
        const auto map = Setting(rules->second, key);
        if (!map.empty() && (i != 0 || Fold_Ascii(map) != Fold_Ascii(start))) {
            result.Maps.push_back(map);
        }
    }
    if (result.Maps.empty()) {
        throw std::runtime_error("Select a map using MapName or MapName00 in [Settings]");
    }
    if (const auto build = Setting(rules->second, "clientbuildnumber"); !build.empty()) {
        uint32 number;
        const auto read = std::from_chars(build.data(), build.data() + build.size(), number);
        if (read.ec != std::errc{} || read.ptr != build.data() + build.size() || number == 0) {
            throw std::runtime_error("ClientBuildNumber must be a positive 32-bit integer");
        }
        result.ClientBuild = number;
        if (!nickname.empty()) {
            result.Join.Owner = Legacy_Ini_String(nickname, 64, "Nickname");
        }
        for (const auto &map : result.Maps) {
            if (map.size() > 27 || Fold_Ascii(fs::path(map).extension().string()) != ".mix") {
                throw std::runtime_error("Client joins require a .mix map name of at most 27 bytes: " + map);
            }
        }
        if (const auto title = Setting(rules->second, "bgametitle"); !title.empty()) {
            result.Join.Title = Legacy_Ini_String(title, 63, "bGameTitle");
        }
        result.Join.Motd = Legacy_Ini_String(Setting(rules->second, "bmotd"), 99, "bMotd");
        if (const auto limit = Setting(rules->second, "timelimitminutes"); !limit.empty()) {
            result.Join.TimeLimit = Read_Integer(limit, 0, std::numeric_limits<int>::max() / 60, "TimeLimitMinutes");
        }
        if (const auto radar = Setting(rules->second, "radarmode"); !radar.empty()) {
            result.Join.RadarMode = Read_Integer(radar, 0, 2, "RadarMode");
        }
        const auto friendly = Fold_Ascii(Setting(rules->second, "isfriendlyfirepermitted"));
        result.Join.FriendlyFire = !friendly.empty() && std::string_view("yt1").find(friendly.front()) != std::string_view::npos;
        const auto loop = Fold_Ascii(Setting(rules->second, "domapsloop"));
        if (!loop.empty() && std::string_view("nf0").find(loop.front()) != std::string_view::npos) {
            result.Join.LoopMaps = false;
        }
        const auto passworded = Fold_Ascii(Setting(rules->second, "ispassworded"));
        if (passworded == "yes" || passworded == "true" || passworded == "1") {
            const auto password = Setting(rules->second, "bpassword");
            if (password.empty() || !Setting(rules->second, "wpassword").empty()) {
                throw std::runtime_error("Passworded sessions require bPassword and an empty wPassword");
            }
            result.Join.Password = Legacy_Ini_String(password, 64, "bPassword");
        } else if (!passworded.empty() && passworded != "no" && passworded != "false" && passworded != "0") {
            throw std::runtime_error("Invalid IsPassworded value");
        }
    }
    return result;
}

std::optional<GameDataFile> Find_Optional_Asset(const fs::path &directory, const std::vector<MixArchive> &archives,
    const std::string &name)
{
    if (const auto loose = Find_Child(directory, name)) {
        return GameDataFile::Loose(*loose);
    }
    for (const auto &archive : archives) {
        if (auto entry = archive.Find(name)) {
            return *entry;
        }
    }
    return std::nullopt;
}

GameDataFile Find_Asset(const fs::path &directory, const std::vector<MixArchive> &archives,
    const std::string &name)
{
    if (auto file = Find_Optional_Asset(directory, archives, name)) {
        return *file;
    }
    throw std::runtime_error("Missing game asset: " + name);
}

void Check_Chunks(const GameDataFile &data, const std::string &name)
{
    auto file = data.Open();
    ChunkLoadClass chunks(file.get());
    constexpr int header_size = sizeof(ChunkHeader);
    int position = 0;
    while (position < data.Length) {
        if (data.Length - position < header_size || !chunks.Open_Chunk() ||
            chunks.Cur_Chunk_Length() > static_cast<unsigned>(data.Length - position - header_size)) {
            throw std::runtime_error("Invalid chunk in " + name + " from " + data.Path.string());
        }
        position += header_size + static_cast<int>(chunks.Cur_Chunk_Length());
        if (!chunks.Close_Chunk() || file->Tell() != position) {
            throw std::runtime_error("Cannot read chunks in " + name + " from " + data.Path.string());
        }
    }
    if (position == 0) {
        throw std::runtime_error("Game asset is empty: " + name);
    }
}

HeadlessArmorTable Load_Armor(const GameDataFile &armor)
{
    auto file = armor.Open();
    std::string text(armor.Length, '\0');
    if (file->Read(text.data(), armor.Length) != armor.Length) {
        throw std::runtime_error("Cannot read armor.ini");
    }
    std::istringstream input(text);
    IniEntryOrder order;
    const auto ini = Read_Ini(input, "armor.ini", &order);
    const auto get = [&](const std::string &section, const std::string &key) {
        const auto found = ini.find(section);
        return found == ini.end() ? std::string{} : Setting(found->second, key);
    };
    const auto coefficient = [&](const std::string &section, const std::string &warhead, float fallback) {
        const auto text = get(section, warhead);
        float value;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value) ? value : fallback;
    };
    std::vector<std::string> warheads;
    for (const auto &key : order["warhead_types"]) {
        warheads.push_back(Fold_Ascii(get("warhead_types", key)));
    }
    HeadlessArmorTable types;
    // The original INI loader uses file order for network indices and separate saved IDs in objects.ddb.
    for (const auto &key : order["armor_types"]) {
        const auto name = Fold_Ascii(get("armor_types", key));
        const auto saved = get("armor_save_ids", name);
        int id = -100;
        const auto result = std::from_chars(saved.data(), saved.data() + saved.size(), id);
        HeadlessArmorType type{result.ec == std::errc{} && result.ptr == saved.data() + saved.size() ? id : -100, {}};
        for (const auto &warhead : warheads) {
            type.Warheads.push_back({coefficient("scale_" + name, warhead, 1.0f),
                coefficient("shield_" + name, warhead, 0.0f)});
        }
        types.push_back(std::move(type));
    }
    return types;
}

std::vector<HeadlessMap> Load_Game_Data(const fs::path &directory, const Settings &settings)
{
    std::vector<MixArchive> core;
    for (const char *name : {"Always2.dat", "Always.dbs", "Always.dat"}) {
        if (const auto path = Find_Child(directory, name)) {
            core.emplace_back(*path);
        }
    }
    const auto definitions = Find_Asset(directory, core, "objects.ddb");
    const auto strings = Find_Asset(directory, core, "strings.tdb");
    Check_Chunks(strings, "strings.tdb");
    const auto armor_data = Find_Asset(directory, core, "armor.ini");
    auto armor = armor_data.Open();
    char first_byte;
    if (armor->Read(&first_byte, 1) != 1) {
        throw std::runtime_error("Cannot read armor.ini");
    }

    const auto armor_table = settings.ClientBuild ? Load_Armor(armor_data) : HeadlessArmorTable{};
    const auto find_core = [&](const std::string &name) { return Find_Optional_Asset(directory, core, name); };
    std::vector<HeadlessMap> maps;
    maps.reserve(settings.Maps.size());
    for (const auto &map : settings.Maps) {
        const auto map_path = Find_Child(directory, map);
        if (!map_path) {
            throw std::runtime_error("Map was not found in the data directory: " + map);
        }
        const auto stem = map_path->stem().string();
        const auto extension = Fold_Ascii(map_path->extension().string());
        std::vector<MixArchive> map_archives;
        if (extension == ".mix") {
            map_archives.emplace_back(*map_path);
        } else if (extension != ".lsd") {
            throw std::runtime_error("Map must be a .mix archive or a loose .lsd file: " + map);
        }
        const auto static_data = Find_Asset(directory, map_archives, stem + ".lsd");
        const auto dynamic_data = Find_Asset(directory, map_archives, stem + ".ldd");
        try {
            const auto map_definitions = Find_Optional_Asset(directory, map_archives, stem + ".ddb");
            auto level = HeadlessLevel::Load(static_data, dynamic_data, definitions, map_definitions);
            auto collision = HeadlessCollision::Load(std::move(level), [&](const std::string &name) {
                auto asset = find_core(name);
                return asset ? asset : Find_Optional_Asset(directory, map_archives, name);
            }, Find_Optional_Asset(directory, map_archives, stem + ".dep"));
            HeadlessPlayerSession::Validate_Map(collision);
            HeadlessJoinData join;
            if (settings.ClientBuild) {
                join = Load_Headless_Join_Data(*settings.ClientBuild, armor_table, strings, definitions,
                    map_definitions, find_core);
            }
            maps.push_back({map, std::move(collision), std::move(join)});
        } catch (const std::runtime_error &error) {
            throw std::runtime_error(map + ": " + error.what());
        }
    }
    return maps;
}

void Report_Static_Collision(const HeadlessCollision &collision)
{
    std::size_t starts = 0;
    std::size_t ray_hits = 0;
    std::size_t box_hits = 0;
    std::size_t box_overlaps = 0;
    // Use the dimensions of the soldier debug box for this startup probe.
    const Vector3 probe_extent(0.3f, 0.3f, 1.0f);
    const Vector3 ray_start(0, 0, 2);
    const Vector3 ray_end(0, 0, -8);
    const Vector3 foot_clearance(0, 0, 0.25f);
    const Vector3 probe_center = foot_clearance + Vector3(0, 0, probe_extent.Z);
    for (const auto &spawn : collision.Level().Spawners()) {
        if (spawn.Enabled && spawn.SoldierStartup && !spawn.Primary) {
            ++starts;
            const auto position = spawn.Transform.Get_Translation();
            const auto ray = collision.Cast_Ray(position + ray_start, position + ray_end, COLLISION_TYPE_PHYSICAL);
            if (ray && !ray->StartsInside) {
                ++ray_hits;
            }
            const AABoxClass probe(position + probe_center, probe_extent);
            if (const auto box = collision.Sweep_Box(probe, ray_end - foot_clearance, COLLISION_TYPE_PHYSICAL)) {
                if (box->StartsInside) {
                    ++box_overlaps;
                } else {
                    ++box_hits;
                }
            }
        }
    }
    std::cout << "Loaded static W3D collision: " << collision.Collider_Count() << " colliders, "
              << collision.Incomplete_Object_Count() << " static objects with incomplete geometry; "
              << ray_hits << '/' << starts << " soldier-start ground rays hit; "
              << box_hits << '/' << starts << " ground box sweeps hit (" << box_overlaps << " initial overlaps)\n";
}

void Report_Soldier_Spawns(const HeadlessCollision &collision)
{
    HeadlessSimulation simulation(collision);
    const char *nod = simulation.Spawn_Soldier(1, PLAYERTYPE_NOD) ? "ready" : "no clear start";
    const char *gdi = simulation.Spawn_Soldier(2, PLAYERTYPE_GDI) ? "ready" : "no clear start";
    std::cout << "Headless soldier spawns: Nod " << nod << ", GDI " << gdi << '\n';
}

volatile std::sig_atomic_t stop_requested = 0;

void Request_Stop(int)
{
    stop_requested = 1;
}

REFUSAL_CODE Refuse_Join_While_Bootstrapping(cPacket &)
{
    return REFUSAL_BY_APPLICATION;
}

void Unexpected_Join(int)
{
    throw std::logic_error("Connection was admitted before a game session was ready");
}

class LanDiscovery {
public:
    using Clock = std::chrono::steady_clock;

    LanDiscovery(const HeadlessJoin &join, in_addr bind_address, std::optional<in_addr> destination) : Join(join)
    {
        if (destination) {
            Targets.push_back({bind_address, *destination});
        } else {
            Find_Broadcast_Targets(bind_address);
        }
        if (Targets.empty()) {
            return;
        }
        cNetUtil::Create_Unbound_Socket(Socket);
        if (Socket != INVALID_SOCKET && !Select_Source(bind_address, destination)) {
            cNetUtil::Close_Socket(Socket);
        }
        if (Socket == INVALID_SOCKET) {
            std::cerr << "FDS: could not open the LAN announcement socket\n";
        } else {
            std::cout << "LAN announcements on UDP port " << Port << '\n';
        }
    }

    LanDiscovery(const LanDiscovery &) = delete;
    LanDiscovery &operator=(const LanDiscovery &) = delete;

    ~LanDiscovery() { cNetUtil::Close_Socket(Socket); }

    void Tick(Clock::time_point now)
    {
        if (Socket != INVALID_SOCKET && now >= NextBroadcast) {
            Send(true);
            NextBroadcast = now + std::chrono::seconds(1);
        }
    }

    void Withdraw()
    {
        if (Sequence != 0) {
            Send(false);
        }
    }

private:
    struct Target {
        in_addr Local;
        in_addr Remote;
    };

    void Find_Broadcast_Targets(in_addr bind_address)
    {
        ifaddrs *list = nullptr;
        if (getifaddrs(&list) != 0) {
            std::cerr << "FDS: could not enumerate interfaces for LAN discovery\n";
            return;
        }
        std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> interfaces(list, freeifaddrs);
        for (auto *entry = interfaces.get(); entry; entry = entry->ifa_next) {
            constexpr unsigned flags = IFF_UP | IFF_BROADCAST;
            if ((entry->ifa_flags & flags) != flags || (entry->ifa_flags & IFF_LOOPBACK) ||
                !entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_broadaddr ||
                entry->ifa_broadaddr->sa_family != AF_INET) {
                continue;
            }
            const auto local = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr;
            if (bind_address.s_addr == INADDR_ANY || bind_address.s_addr == local.s_addr) {
                const auto remote = reinterpret_cast<const sockaddr_in *>(entry->ifa_broadaddr)->sin_addr;
                Targets.push_back({local, remote});
            }
        }
    }

    bool Select_Source(in_addr bind_address, std::optional<in_addr> destination)
    {
        sockaddr_in local{};
        cNetUtil::Create_Local_Address(&local, 0);
        if (bind_address.s_addr != INADDR_ANY) {
            local.sin_addr = bind_address;
            return bind(Socket, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) == 0;
        }
        if (destination) {
            const auto remote = Destination(*destination);
            socklen_t size = sizeof(local);
            if (connect(Socket, reinterpret_cast<const sockaddr *>(&remote), sizeof(remote)) != 0 ||
                getsockname(Socket, reinterpret_cast<sockaddr *>(&local), &size) != 0) {
                return false;
            }
            Targets.front().Local = local.sin_addr;
        }
        return true;
    }

    static sockaddr_in Destination(in_addr address)
    {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(Port);
        remote.sin_addr = address;
        return remote;
    }

    void Send(bool hosting)
    {
        ++Sequence;
        for (const auto &target : Targets) {
            cPacket packet;
            Join.Write_Lan_Announcement(packet, target.Local.s_addr, Sequence, hosting);
            const auto remote = Destination(target.Remote);
            sendto(Socket, packet.Get_Data(), packet.Get_Compressed_Size_Bytes(), 0,
                reinterpret_cast<const sockaddr *>(&remote), sizeof(remote));
        }
    }

    static constexpr unsigned short Port = 3373; // cLanChat::LAN_PORT.
    const HeadlessJoin &Join;
    std::vector<Target> Targets;
    SOCKET Socket = INVALID_SOCKET;
    Clock::time_point NextBroadcast{};
    uint32 Sequence = 0;
};

int Run_Headless(const Settings &settings, const Options &options, const in_addr &bind_address,
    const std::vector<HeadlessMap> &maps)
{
    if (std::signal(SIGINT, Request_Stop) == SIG_ERR || std::signal(SIGTERM, Request_Stop) == SIG_ERR) {
        throw std::runtime_error("Cannot install shutdown handlers");
    }
    if (!cNetUtil::Protocol_Init(false)) {
        throw std::runtime_error("IPv4 UDP is unavailable");
    }
    PacketManager.Set_Is_Server(true);
    cConnection server;
    if (!server.Init_As_Server(settings.Port, settings.MaxPlayers, true, ntohl(bind_address.s_addr), false)) {
        throw std::runtime_error("Cannot bind UDP to " + options.BindAddress + ":" + std::to_string(settings.Port));
    }

    HeadlessPlayerSession session(maps.front().Collision, server);
    auto join_options = settings.Join;
    join_options.Address = bind_address.s_addr;
    std::optional<HeadlessJoin> join;
    std::optional<LanDiscovery> discovery;
    if (settings.ClientBuild) {
        server.Set_Bandwidth_Budget_Out(2000000); // Original BANDWIDTH_LANT1.
        join.emplace(server, session, maps, join_options);
        discovery.emplace(*join, bind_address, options.LanDestination);
        server.Install_Application_Acceptance_Handler([&](cPacket &packet) { return join->Accept(packet); });
        server.Install_Conn_Handler([&](int peer_id) { join->Connected(peer_id); });
    } else {
        server.Install_Application_Acceptance_Handler(Refuse_Join_While_Bootstrapping);
        server.Install_Conn_Handler(Unexpected_Join);
    }
    server.Install_Server_Packet_Handler([&](cPacket &packet, int peer_id) {
        if (join) {
            join->Receive(packet, peer_id);
        } else {
            packet.Flush();
        }
    });
    const auto disconnect = [&](int peer_id) {
        if (join) {
            join->Disconnected(peer_id);
        }
    };
    server.Install_Server_Broken_Connection_Handler(disconnect);
    server.Install_Eviction_Handler([&](int peer_id) {
        server.Destroy_Connection(peer_id);
        disconnect(peer_id);
    });
    std::cout << "FDS bootstrap listening on " << options.BindAddress << ':' << server.Get_Local_Port()
              << (join ? " (experimental client joins enabled)" : " (ClientBuildNumber unset; joins refused)")
              << std::endl;
    auto previous = HeadlessSimulation::Clock::now();
    while (!stop_requested) {
        const auto frame_start = HeadlessSimulation::Clock::now();
        if (join && !join->Update_Round(frame_start)) {
            break;
        }
        server.Service_Read();
        if (join) {
            join->Close_Pending();
        }
        session.Advance(frame_start - previous);
        if (join) {
            join->Update_Respawns();
        }
        previous = frame_start;
        server.Service_Send(true);
        if (discovery) {
            discovery->Tick(frame_start);
        }
        std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(10));
    }
    if (discovery) {
        discovery->Withdraw();
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = Parse_Options(argc, argv);
        if (options.Help) {
            std::cout << "Usage: renegade_fds [--config FILE] [--data-dir DIR] [--bind IPV4] [--port PORT]\n"
                         "                    [--lan-destination IPV4] [--check]\n"
                         "  --check       Check configuration and map data without opening a socket.\n"
                         "  --bind IPV4   Listen on this IPv4 address (default: 127.0.0.1).\n"
                         "  --port PORT   Override the INI port (0 selects a free port).\n"
                         "  --lan-destination IPV4   Send LAN announcements to this address on UDP port 3373.\n";
            return 0;
        }
        in_addr bind_address{};
        if (inet_pton(AF_INET, options.BindAddress.c_str(), &bind_address) != 1) {
            throw std::runtime_error("--bind must be a numeric IPv4 address");
        }
        const auto directory = Resolve_Data_Directory(options.DataDir);
        const Settings settings = Load_Settings(options, directory);
        const auto maps = Load_Game_Data(directory, settings);
        const auto &collision = maps.front().Collision;
        const auto &level = collision.Level();
        std::cout << "Validated game data for " << settings.Maps.size() << " map(s); first map: "
                  << settings.Maps.front() << '\n'
                  << "Loaded headless scene: " << level.Static_Objects().size() << " static objects in "
                  << level.Node_Count() << " spatial nodes, " << level.Buildings().size() << " buildings, "
                  << level.Spawners().size() << " spawners (" << level.Count_Player_Spawns(PLAYERTYPE_NOD)
                  << " Nod and " << level.Count_Player_Spawns(PLAYERTYPE_GDI) << " GDI soldier starts)\n";
        Report_Static_Collision(collision);
        if (options.CheckOnly) {
            for (const auto &map : maps) {
                if (maps.size() > 1) {
                    std::cout << map.Name << '\n';
                }
                Report_Soldier_Spawns(map.Collision);
            }
            if (settings.ClientBuild) {
                std::cout << "Client version key: " << maps.front().Join.VersionKey << '\n';
            }
            return 0;
        }
        return Run_Headless(settings, options, bind_address, maps);
    } catch (const std::exception &error) {
        std::cerr << "FDS: " << error.what() << '\n';
        return 1;
    }
}
