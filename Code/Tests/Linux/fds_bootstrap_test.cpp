// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "connect.h"
#include "crc.h"
#include "netutil.h"

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
        Write_Archive(data / "always.dbs", {{"STRINGS.TDB", chunk}, {"OBJECTS.DDB", chunk}});
        std::ofstream(data / "ARMOR.ini") << "[Armor]\n";
        Write_Archive(data / "C&C_Test.mix", {{"C&C_TEST.LDD", chunk}, {"C&C_TEST.LSD", chunk}});
        std::ofstream(master) << "[Server]\r\nConfig=settings.ini\r\nGameType=LAN\r\nPort=0\r\n";
        std::ofstream(settings) << "[settings]\nMapName=c&c_test.MIX ; first map\nMaxPlayers=16\n";

        FdsProcess check(argv[1], fixture.Path, master, "--check");
        const auto check_status = check.Wait();
        Check(check_status == 0, "FDS rejected valid fixture: " + check.Output());
        Check(check.Output().find("Validated game data for 1 map(s)") != std::string::npos,
            "FDS did not validate the configured map");

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
            broken.seekp(16 + 2 * chunk.size() + 8);
            Write_U32(broken, 0xffffff00u);
        }
        FdsProcess bad_mix(argv[1], fixture.Path, master, "--check");
        Check(bad_mix.Wait() == 1 && bad_mix.Output().find("Invalid MIX1 index entry") != std::string::npos,
            "FDS accepted an invalid map archive");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
