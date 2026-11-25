// SPDX-License-Identifier: GPL-3.0-or-later
#include "mixarchive.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#include "crc.h"

namespace {

class BoundedGameFile final : public FileClass {
public:
    explicit BoundedGameFile(const GameDataFile &file)
        : Name(file.Path.string()), Base(file.Offset), Length(file.Length), ViewLength(file.Length) {}

    const char *File_Name() const override { return Name.c_str(); }
    const char *Set_Name(const char *name) override { return Name == name ? File_Name() : nullptr; }

    int Open(const char *name, int rights) override { return Set_Name(name) && Open(rights); }
    int Open(int rights) override
    {
        Close();
        if (rights != READ || !Is_Available(0)) {
            return 0;
        }
        Stream.open(Name, std::ios::binary);
        if (Stream && Seek(0, SEEK_SET) == 0) {
            return 1;
        }
        Close();
        return 0;
    }

    void Close() override
    {
        if (Stream.is_open()) {
            Stream.close();
        }
        Stream.clear();
        Position = 0;
    }

    bool Is_Available(int) override
    {
        std::error_code error;
        auto size = std::filesystem::file_size(Name, error);
        return !error && Length >= 0 && Base <= size && std::uint64_t(Length) <= size - Base;
    }

    bool Is_Open() const override { return Stream.is_open(); }
    int Size() override { return ViewLength; }

    int Read(void *buffer, int count) override
    {
        if (!Is_Open() || count <= 0) {
            return 0;
        }
        count = std::min(count, ViewLength - Position);
        Stream.read(static_cast<char *>(buffer), count);
        int read = static_cast<int>(Stream.gcount());
        Position += read;
        return read;
    }

    int Seek(int offset, int origin) override
    {
        std::int64_t next = offset;
        if (origin == SEEK_CUR) {
            next += Position;
        } else if (origin == SEEK_END) {
            next += ViewLength;
        } else if (origin != SEEK_SET) {
            return -1;
        }
        if (!Is_Open() || next < 0 || next > ViewLength) {
            return -1;
        }
        Stream.clear();
        Stream.seekg(static_cast<std::streamoff>(Base + ViewStart + next));
        if (!Stream) {
            return -1;
        }
        Position = static_cast<int>(next);
        return Position;
    }

    void Bias(int start, int length) override
    {
        if (start < 0 || start > Length || length < -1 || length > Length - start) {
            throw std::out_of_range("Invalid slice in " + Name);
        }
        ViewStart = start;
        ViewLength = length == -1 ? Length - start : length;
        Position = 0;
        if (Is_Open()) {
            Seek(0, SEEK_SET);
        }
    }

    int Create() override { return 0; }
    int Delete() override { return 0; }
    int Write(const void *, int) override { return 0; }
    void Error(int error, int, const char *) override
    {
        throw std::runtime_error(Name + ": file error " + std::to_string(error));
    }

private:
    std::string Name;
    std::ifstream Stream;
    std::uint64_t Base;
    int Length;
    int ViewStart = 0;
    int ViewLength;
    int Position = 0;
};

std::uint32_t Read_U32(const unsigned char *bytes)
{
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

} // namespace

std::unique_ptr<FileClass> GameDataFile::Open() const
{
    auto reader = std::make_unique<BoundedGameFile>(*this);
    if (!reader->Open(FileClass::READ)) {
        throw std::runtime_error("Cannot read game data from " + Path.string());
    }
    return reader;
}

GameDataFile GameDataFile::Loose(const std::filesystem::path &path)
{
    auto length = std::filesystem::file_size(path);
    if (length > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Game data file is too large: " + path.string());
    }
    return {path, 0, static_cast<int>(length)};
}

MixArchive::MixArchive(std::filesystem::path path) : Path(std::move(path))
{
    std::ifstream input(Path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    input.seekg(0);
    std::array<unsigned char, 16> header;
    if (!input.read(reinterpret_cast<char *>(header.data()), header.size()) ||
        std::memcmp(header.data(), "MIX1", 4) != 0) {
        throw std::runtime_error("Invalid MIX1 header: " + Path.string());
    }

    const auto index = Read_U32(header.data() + 4);
    const auto names = Read_U32(header.data() + 8);
    const auto file_length = static_cast<std::uint64_t>(length);
    if (index < header.size() || index > file_length || file_length - index < 4 ||
        (names != 0 && (names < std::uint64_t(index) + 4 || names > file_length || file_length - names < 4))) {
        throw std::runtime_error("Invalid MIX1 index or names offset: " + Path.string());
    }

    input.seekg(index);
    std::array<unsigned char, 12> entry;
    if (!input.read(reinterpret_cast<char *>(entry.data()), 4)) {
        throw std::runtime_error("Cannot read MIX1 entry count: " + Path.string());
    }
    const auto count = Read_U32(entry.data());
    const auto index_end = names == 0 ? file_length : names;
    if (count > (index_end - index - 4) / entry.size()) {
        throw std::runtime_error("Invalid MIX1 entry count: " + Path.string());
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        if (!input.read(reinterpret_cast<char *>(entry.data()), entry.size())) {
            throw std::runtime_error("Truncated MIX1 index: " + Path.string());
        }
        Entry item{Read_U32(entry.data()), Read_U32(entry.data() + 4), Read_U32(entry.data() + 8)};
        if (item.Offset < header.size() || item.Offset > index || item.Length > index - item.Offset ||
            item.Length > std::numeric_limits<int>::max() || (!Entries.empty() && item.Crc < Entries.back().Crc)) {
            throw std::runtime_error("Invalid MIX1 index entry: " + Path.string());
        }
        Entries.push_back(item);
    }
}

std::optional<GameDataFile> MixArchive::Find(std::string_view name) const
{
    std::string key(name);
    for (char &byte : key) {
        if (byte >= 'a' && byte <= 'z') {
            byte = static_cast<char>(byte - 'a' + 'A');
        }
    }
    const auto crc = CRC::String(key.c_str());
    const auto found = std::lower_bound(Entries.begin(), Entries.end(), crc,
        [](const Entry &entry, std::uint32_t key) { return entry.Crc < key; });
    if (found == Entries.end() || found->Crc != crc) {
        return std::nullopt;
    }
    return GameDataFile{Path, found->Offset, static_cast<int>(found->Length)};
}
