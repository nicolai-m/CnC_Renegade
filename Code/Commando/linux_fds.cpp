// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "chunkio.h"
#include "connect.h"
#include "headless_level.h"
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
        if (option != "--config" && option != "--data-dir" && option != "--bind" && option != "--port") {
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
        } else {
            options.Port = Read_Port(argv[i], "--port");
        }
    }
    return options;
}

using IniSection = std::map<std::string, std::string>;
using IniFile = std::map<std::string, IniSection>;

IniFile Read_Ini(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read configuration: " + path.string());
    }

    IniFile result;
    IniSection *section = nullptr;
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
            section = &result[Fold_Ascii(Trim(text.substr(1, text.size() - 2)))];
            continue;
        }
        const auto equals = text.find('=');
        if (section != nullptr && equals != std::string_view::npos) {
            const auto key = Trim(text.substr(0, equals));
            if (!key.empty()) {
                (*section)[Fold_Ascii(key)] = Trim(text.substr(equals + 1));
            }
        }
    }
    if (input.bad()) {
        throw std::runtime_error("Error reading configuration: " + path.string());
    }
    return result;
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
};

Settings Load_Settings(const Options &options, const fs::path &data_dir)
{
    auto ini = Read_Ini(options.Config);
    std::string port;
    const auto server = ini.find("server");
    if (server != ini.end()) {
        const auto game_type = Fold_Ascii(Setting(server->second, "gametype"));
        if (!game_type.empty() && game_type != "lan") {
            throw std::runtime_error("Only GameType=LAN is supported by the Linux bootstrap");
        }
        port = Setting(server->second, "port");
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

HeadlessLevel Load_Game_Data(const fs::path &directory, const Settings &settings)
{
    std::vector<MixArchive> core;
    for (const char *name : {"Always2.dat", "Always.dbs", "Always.dat"}) {
        if (const auto path = Find_Child(directory, name)) {
            core.emplace_back(*path);
        }
    }
    const auto definitions = Find_Asset(directory, core, "objects.ddb");
    Check_Chunks(Find_Asset(directory, core, "strings.tdb"), "strings.tdb");
    auto armor = Find_Asset(directory, core, "armor.ini").Open();
    char first_byte;
    if (armor->Read(&first_byte, 1) != 1) {
        throw std::runtime_error("Cannot read armor.ini");
    }

    std::optional<HeadlessLevel> level;
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
        if (!level) {
            level = HeadlessLevel::Load(static_data, dynamic_data, definitions,
                Find_Optional_Asset(directory, map_archives, stem + ".ddb"));
        } else {
            Check_Chunks(static_data, stem + ".lsd");
            Check_Chunks(dynamic_data, stem + ".ldd");
        }
    }
    return std::move(*level);
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

int Run_Headless(const Settings &settings, const std::string &address, const in_addr &bind_address)
{
    if (std::signal(SIGINT, Request_Stop) == SIG_ERR || std::signal(SIGTERM, Request_Stop) == SIG_ERR) {
        throw std::runtime_error("Cannot install shutdown handlers");
    }
    if (!cNetUtil::Protocol_Init(false)) {
        throw std::runtime_error("IPv4 UDP is unavailable");
    }
    PacketManager.Set_Is_Server(true);
    cConnection server;
    server.Install_Application_Acceptance_Handler(Refuse_Join_While_Bootstrapping);
    server.Install_Conn_Handler(Unexpected_Join);
    if (!server.Init_As_Server(settings.Port, settings.MaxPlayers, true, ntohl(bind_address.s_addr), false)) {
        throw std::runtime_error("Cannot bind UDP to " + address + ":" + std::to_string(settings.Port));
    }

    std::cout << "FDS bootstrap listening on " << address << ':' << server.Get_Local_Port()
              << " (game session pending; joins refused)" << std::endl;
    while (!stop_requested) {
        server.Service_Read();
        server.Service_Send(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = Parse_Options(argc, argv);
        if (options.Help) {
            std::cout << "Usage: renegade_fds [--config FILE] [--data-dir DIR] [--bind IPV4] [--port PORT] [--check]\n"
                         "  --check       Check configuration and map data without opening a socket.\n"
                         "  --bind IPV4   Listen on this IPv4 address (default: 127.0.0.1).\n"
                         "  --port PORT   Override the INI port (0 selects a free port).\n";
            return 0;
        }
        in_addr bind_address{};
        if (inet_pton(AF_INET, options.BindAddress.c_str(), &bind_address) != 1) {
            throw std::runtime_error("--bind must be a numeric IPv4 address");
        }
        const auto directory = Resolve_Data_Directory(options.DataDir);
        const Settings settings = Load_Settings(options, directory);
        const auto level = Load_Game_Data(directory, settings);
        std::cout << "Validated game data for " << settings.Maps.size() << " map(s); first map: "
                  << settings.Maps.front() << '\n'
                  << "Loaded headless scene: " << level.Static_Objects().size() << " static objects in "
                  << level.Node_Count() << " spatial nodes, " << level.Buildings().size() << " buildings, "
                  << level.Spawners().size() << " spawners (" << level.Count_Player_Spawns(PLAYERTYPE_NOD)
                  << " Nod and " << level.Count_Player_Spawns(PLAYERTYPE_GDI) << " GDI soldier starts)"
                  << std::endl;
        return options.CheckOnly ? 0 : Run_Headless(settings, options.BindAddress, bind_address);
    } catch (const std::exception &error) {
        std::cerr << "FDS: " << error.what() << '\n';
        return 1;
    }
}
