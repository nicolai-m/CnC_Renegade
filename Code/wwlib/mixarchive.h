// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MIXARCHIVE_H
#define MIXARCHIVE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "wwfile.h"

struct GameDataFile {
    std::filesystem::path Path;
    std::uint64_t Offset;
    int Length;

    // Open a bounded, read-only FileClass for use with ChunkLoadClass.
    std::unique_ptr<FileClass> Open() const;
    static GameDataFile Loose(const std::filesystem::path &path);
};

class MixArchive {
public:
    explicit MixArchive(std::filesystem::path path);
    std::optional<GameDataFile> Find(std::string_view name) const;

private:
    struct Entry {
        std::uint32_t Crc;
        std::uint32_t Offset;
        std::uint32_t Length;
    };

    std::filesystem::path Path;
    std::vector<Entry> Entries;
};

#endif // MIXARCHIVE_H
