// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_join_data.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

#include "bounded_chunk_reader.h"
#include "combatchunkid.h"
#include "crc.h"
#include "playertype.h"
#include "renegade_version_files.h"
#include "saveloadids.h"

namespace {

// DefinitionMgrClass stores the original persist factory records.
constexpr uint32 DefinitionObjects = 0x101;
constexpr uint32 FactoryData = 0x00100101;
constexpr uint32 DefinitionIdentity = 0x100;
constexpr uint32 HumanPhysicsFactory = 0x20501;
constexpr uint32 PhysicsModelVariables = 0x055ffe08;
constexpr uint32 ArmedPhysicalParent = 418001829;
constexpr uint32 PhysicalVariables = 909991657;
constexpr uint32 DefenseVariables = 7311607;
constexpr uint32 TranslateVariables = 0x07141200;
constexpr float MaxDefense = 2000.0f;

std::string Fold_Ascii(std::string name)
{
    for (char &c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 'a' - 'A');
        }
    }
    return name;
}

struct Record {
    HeadlessSoldierPreset Soldier;
    std::string Name;
    int PhysicsId = 0;
};

void Read_Identity(BoundedChunkReader &reader, Record &record)
{
    reader.Each_Micro([&](uint32 field) {
        if (field == 1) {
            record.Soldier.DefinitionId = reader.Value<sint32>();
        } else if (field == 3) {
            record.Name = reader.Read_String();
        }
    });
}

int Read_Defense_Value(BoundedChunkReader &reader)
{
    const float value = reader.Value<float>();
    reader.Require(std::isfinite(value) && value >= 0 && value <= MaxDefense, "Invalid soldier defense value");
    return static_cast<int>(std::lround(value));
}

void Read_Record(BoundedChunkReader &reader, Record &record, uint32 parent, unsigned depth = 0)
{
    reader.Require(depth < 20, "Soldier definition is nested too deeply");
    reader.Each_Chunk([&](uint32 id) {
        if (id == DefinitionIdentity) {
            Read_Identity(reader, record);
        } else if (id == PhysicalVariables && parent == ArmedPhysicalParent) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 18) {
                    record.PhysicsId = reader.Value<sint32>();
                }
            });
        } else if (id == DefenseVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 0) {
                    record.Soldier.Health = Read_Defense_Value(reader);
                } else if (field == 3) {
                    record.Soldier.Shield = Read_Defense_Value(reader);
                } else if (field == 5) {
                    record.Soldier.ShieldType = reader.Value<uint32>();
                }
            });
        } else if (id == PhysicsModelVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 1) {
                    const auto path = reader.Read_String();
                    const auto basename = path.substr(path.find_last_of("/\\") + 1);
                    const auto extension = basename.find_last_of('.');
                    record.Soldier.Model = basename.substr(0, extension);
                }
            });
        } else if (reader.Has_Children()) {
            Read_Record(reader, record, id, depth + 1);
        }
    });
}

void Load_Presets(const GameDataFile &source, std::map<int, std::string> &physics, std::array<Record, 2> &soldiers)
{
    BoundedChunkReader reader(source);
    reader.In_Chunk(CHUNKID_SAVELOAD_DEFMGR, [&] {
        reader.In_Chunk(DefinitionObjects, [&] {
            reader.Each_Chunk([&](uint32 factory) {
                if (factory != CHUNKID_GAME_OBJECT_DEF_SOLDIER && factory != HumanPhysicsFactory) {
                    return;
                }
                Record record;
                reader.In_Chunk(FactoryData, [&] { Read_Record(reader, record, FactoryData); });
                if (factory == HumanPhysicsFactory) {
                    physics[record.Soldier.DefinitionId] = std::move(record.Soldier.Model);
                } else {
                    const auto name = Fold_Ascii(record.Name);
                    if (name == "cnc_nod_minigunner_0") {
                        soldiers[PLAYERTYPE_NOD] = std::move(record);
                    } else if (name == "cnc_gdi_minigunner_0") {
                        soldiers[PLAYERTYPE_GDI] = std::move(record);
                    }
                }
            });
        });
    });
}

uint32 Read_Strings_Version(const GameDataFile &source)
{
    BoundedChunkReader reader(source);
    std::optional<uint32> version;
    reader.In_Chunk(CHUNKID_WWTRANSLATEDB_BEGIN, [&] {
        reader.In_Chunk(TranslateVariables, [&] {
            reader.Each_Micro([&](uint32 field) {
                if (field == 1) {
                    version = reader.Value<uint32>();
                }
            });
        });
    });
    reader.Require(version.has_value(), "Missing strings.tdb version");
    return *version;
}

uint32 Data_CRC(const HeadlessCollision::FindAsset &find_asset)
{
    uint32 crc = 0;
    for (const auto *name : RenegadeVersionFiles) {
        if (const auto source = find_asset(name)) {
            auto file = source->Open();
            std::array<unsigned char, 4096> buffer;
            for (int remaining = source->Length; remaining > 0;) {
                const auto count = std::min<int>(remaining, buffer.size());
                if (file->Read(buffer.data(), count) != count) {
                    throw std::runtime_error(std::string("Cannot read version asset: ") + name);
                }
                crc = CRC::Memory(buffer.data(), count, crc);
                remaining -= count;
            }
        }
    }
    return crc;
}

uint32 Build_CRC(const std::string &text)
{
    return static_cast<uint32>(CRCEngine()(text.data(), static_cast<int>(text.size())));
}

} // namespace

HeadlessJoinData Load_Headless_Join_Data(uint32 client_build, unsigned armor_types,
    const GameDataFile &strings, const GameDataFile &definitions, const std::optional<GameDataFile> &map_definitions,
    const HeadlessCollision::FindAsset &find_core_asset)
{
    HeadlessJoinData result;
    const auto exe = "RENEGADE " + std::to_string(client_build);
    const auto tdb = "strings.tdb " + std::to_string(Read_Strings_Version(strings));
    result.ExeCRC = Build_CRC(exe);
    result.StringsCRC = Build_CRC(tdb);
    result.VersionKey = Build_CRC(exe + " " + tdb + " ") ^ Data_CRC(find_core_asset);
    result.ArmorTypes = armor_types;

    std::array<Record, 2> soldiers;
    std::map<int, std::string> physics;
    Load_Presets(definitions, physics, soldiers);
    if (map_definitions) {
        Load_Presets(*map_definitions, physics, soldiers);
    }
    for (std::size_t team = 0; team < soldiers.size(); ++team) {
        const auto &record = soldiers[team];
        const auto model = physics.find(record.PhysicsId);
        if (record.Soldier.DefinitionId <= 0 || record.Soldier.Health <= 0 ||
            armor_types == 0 || record.Soldier.ShieldType >= armor_types ||
            model == physics.end() || model->second.empty()) {
            throw std::runtime_error("Missing or invalid Nod/GDI minigunner preset");
        }
        result.Soldiers[team] = record.Soldier;
        result.Soldiers[team].Model = model->second;
    }
    return result;
}
