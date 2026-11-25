// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOUNDED_CHUNK_READER_H
#define BOUNDED_CHUNK_READER_H

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "chunkio.h"
#include "mixarchive.h"

// Keep each read bounded by the enclosing chunk while using the original
// ChunkLoadClass for its header and microchunk handling.
class BoundedChunkReader {
public:
    explicit BoundedChunkReader(const GameDataFile &source)
        : File(source.Open()), Chunks(File.get()), End(source.Length) {}

    template<class Visit> void Each_Chunk(Visit visit)
    {
        const int limit = End;
        while (File->Tell() < limit) {
            Require(limit - File->Tell() >= static_cast<int>(sizeof(ChunkHeader)) && Chunks.Open_Chunk(),
                "Invalid chunk header");
            Set_Child_End(Chunks.Cur_Chunk_Length(), limit);
            visit(Chunks.Cur_Chunk_ID());
            Require(Chunks.Close_Chunk() && File->Tell() == End, "Cannot finish chunk");
            End = limit;
        }
    }

    template<class Visit> void In_Chunk(uint32 id, Visit visit)
    {
        bool seen = false;
        Each_Chunk([&](uint32 child) {
            if (child == id) {
                Require(!seen, "Duplicate required chunk " + std::to_string(id));
                seen = true;
                visit();
            }
        });
        Require(seen, "Missing required chunk " + std::to_string(id));
    }

    template<class Visit> void Each_Micro(Visit visit)
    {
        const int limit = End;
        while (File->Tell() < limit) {
            Require(limit - File->Tell() >= static_cast<int>(sizeof(MicroChunkHeader)) && Chunks.Open_Micro_Chunk(),
                "Invalid microchunk header");
            Set_Child_End(Chunks.Cur_Micro_Chunk_Length(), limit);
            visit(Chunks.Cur_Micro_Chunk_ID());
            Require(Chunks.Close_Micro_Chunk() && File->Tell() == End, "Cannot finish microchunk");
            End = limit;
        }
    }

    template<class T> T Value()
    {
        T result{};
        Require(End - File->Tell() == static_cast<int>(sizeof(result)) &&
            Chunks.Read(&result, sizeof(result)) == sizeof(result), "Invalid field size");
        return result;
    }

    std::string Read_String()
    {
        const int size = End - File->Tell();
        Require(size > 0, "Empty string field");
        std::string result(size, '\0');
        Require(Chunks.Read(result.data(), size) == static_cast<uint32>(size) &&
            result.find('\0') == result.size() - 1, "Invalid terminated string field");
        result.pop_back();
        return result;
    }

    template<class T> std::vector<T> Array()
    {
        const auto bytes = static_cast<std::size_t>(End - File->Tell());
        Require(bytes % sizeof(T) == 0, "Invalid array size");
        std::vector<T> values(bytes / sizeof(T));
        if (bytes != 0) {
            Require(Chunks.Read(values.data(), bytes) == bytes, "Truncated array");
        }
        return values;
    }

    bool Has_Children() { return Chunks.Contains_Chunks() != 0; }

    void Require(bool condition, const std::string &message) const
    {
        if (!condition) {
            throw std::runtime_error(std::string(File->File_Name()) + ": " + message);
        }
    }

private:
    void Set_Child_End(uint32 size, int limit)
    {
        const int start = File->Tell();
        Require(start <= limit && size <= static_cast<uint32>(limit - start), "Chunk extends past its container");
        End = start + static_cast<int>(size);
    }

    std::unique_ptr<FileClass> File;
    ChunkLoadClass Chunks;
    int End;
};

#endif // BOUNDED_CHUNK_READER_H
