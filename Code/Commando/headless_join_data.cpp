// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_join_data.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include "bounded_chunk_reader.h"
#include "combatchunkid.h"
#include "crc.h"
#include "playertype.h"
#include "renegade_version_files.h"
#include "saveloadids.h"
#include "wwpacket.h"

namespace {

// DefinitionMgrClass stores the original persist factory records.
constexpr uint32 DefinitionObjects = 0x101;
constexpr uint32 FactoryData = 0x00100101;
constexpr uint32 DefinitionIdentity = 0x100;
constexpr uint32 HumanPhysicsFactory = 0x20501;
constexpr uint32 PhysicsModelVariables = 0x055ffe08;
constexpr uint32 ArmedPhysicalParent = 418001829;
constexpr uint32 ArmedVariables = 418001830;
constexpr uint32 PhysicalVariables = 909991657;
constexpr uint32 DefenseVariables = 7311607;
constexpr uint32 ScriptList = 627001057;
constexpr uint32 WeaponVariables = 1205091654;
constexpr uint32 WeaponParent = 1205091655;
constexpr uint32 PowerupVariables = 909991657;
constexpr uint32 TranslateVariables = 0x07141200;
constexpr float MaxDefense = 2000.0f;
// Reserve the fixed soldier fields, including 32-bit position codes and airborne velocity.
constexpr std::size_t SoldierPacketFixedBytes = 128;

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
    std::array<int, 2> WeaponIds{};
    int WeaponRounds = -1;
    std::vector<std::string> SpawnPowerups;
};

struct WeaponDefinition {
    float KeyNumber = 0;
    int ClipSize = 0;
    int MaxInventoryRounds = 100;
};

struct WeaponGrant {
    int DefinitionId = 0;
    int Rounds = 0;
    bool GiveWeapon = true;
};

struct EquipmentDefinitions {
    std::map<int, WeaponDefinition> Weapons;
    std::map<std::string, WeaponGrant> Powerups;
};

void Read_Identity(BoundedChunkReader &reader, int &definition_id, std::string &name)
{
    reader.Each_Micro([&](uint32 field) {
        if (field == 1) {
            definition_id = reader.Value<sint32>();
        } else if (field == 3) {
            name = reader.Read_String();
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
            Read_Identity(reader, record.Soldier.DefinitionId, record.Name);
        } else if (id == ArmedVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 11) {
                    record.WeaponIds[0] = reader.Value<sint32>();
                } else if (field == 12) {
                    record.WeaponRounds = reader.Value<sint32>();
                } else if (field == 14) {
                    record.WeaponIds[1] = reader.Value<sint32>();
                }
            });
        } else if (id == ScriptList) {
            std::string script;
            reader.Each_Micro([&](uint32 field) {
                if (field == 2) {
                    script = Fold_Ascii(reader.Read_String());
                } else if (field == 3 && script == "m00_grantpowerup_created") {
                    record.SpawnPowerups.push_back(Fold_Ascii(reader.Read_String()));
                }
            });
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

void Read_Weapon(BoundedChunkReader &reader, EquipmentDefinitions &equipment)
{
    int definition_id = 0;
    std::string name;
    WeaponDefinition weapon;
    reader.Each_Chunk([&](uint32 id) {
        if (id == WeaponParent) {
            reader.In_Chunk(DefinitionIdentity, [&] { Read_Identity(reader, definition_id, name); });
        } else if (id == WeaponVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 29) {
                    weapon.ClipSize = reader.Value<sint32>();
                } else if (field == 35) {
                    weapon.MaxInventoryRounds = reader.Value<sint32>();
                } else if (field == 37) {
                    const float key = reader.Value<float>();
                    weapon.KeyNumber = std::isfinite(key) ? key : 0;
                }
            });
        }
    });
    if (definition_id > 0) {
        equipment.Weapons[definition_id] = weapon;
    }
}

void Read_Powerup(BoundedChunkReader &reader, WeaponGrant &grant, std::string &name, unsigned depth = 0)
{
    reader.Require(depth < 20, "Powerup definition is nested too deeply");
    reader.Each_Chunk([&](uint32 id) {
        if (id == DefinitionIdentity) {
            int definition_id = 0;
            Read_Identity(reader, definition_id, name);
        } else if (depth == 0 && id == PowerupVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 8) {
                    grant.DefinitionId = reader.Value<sint32>();
                } else if (field == 9) {
                    grant.GiveWeapon = reader.Value<uint8>() != 0;
                } else if (field == 10) {
                    grant.Rounds = reader.Value<sint32>();
                }
            });
        } else if (reader.Has_Children()) {
            Read_Powerup(reader, grant, name, depth + 1);
        }
    });
}

void Load_Presets(const GameDataFile &source, std::map<int, std::string> &physics, std::array<Record, 2> &soldiers,
    EquipmentDefinitions &equipment)
{
    BoundedChunkReader reader(source);
    reader.In_Chunk(CHUNKID_SAVELOAD_DEFMGR, [&] {
        reader.In_Chunk(DefinitionObjects, [&] {
            reader.Each_Chunk([&](uint32 factory) {
                if (factory == CHUNKID_WEAPON_DEF) {
                    reader.In_Chunk(FactoryData, [&] { Read_Weapon(reader, equipment); });
                    return;
                }
                if (factory == CHUNKID_GAME_OBJECT_DEF_POWERUP) {
                    std::string name;
                    WeaponGrant grant;
                    reader.In_Chunk(FactoryData, [&] { Read_Powerup(reader, grant, name); });
                    if (!name.empty()) {
                        equipment.Powerups[Fold_Ascii(std::move(name))] = grant;
                    }
                    return;
                }
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

void Load_Starting_Weapons(const Record &record, const EquipmentDefinitions &equipment, HeadlessSoldierPreset &soldier)
{
    struct LoadedWeapon {
        int DefinitionId;
        int Clip = 0;
        int Inventory = 0;
        bool Owned = false;
    };
    std::vector<LoadedWeapon> inventory;
    const auto grant_weapon = [&](const WeaponGrant &grant) {
        const auto def = equipment.Weapons.find(grant.DefinitionId);
        if (def == equipment.Weapons.end()) {
            return false;
        }
        auto found = std::find_if(inventory.begin(), inventory.end(), [&](const auto &weapon) {
            return weapon.DefinitionId == grant.DefinitionId;
        });
        if (found == inventory.end()) {
            inventory.push_back({grant.DefinitionId});
            found = std::prev(inventory.end());
        }
        found->Owned |= grant.GiveWeapon;
        const int clip_size = std::max(0, def->second.ClipSize);
        if (grant.Rounds < 0) {
            if (found->Clip == 0) {
                found->Clip = clip_size;
            }
            found->Inventory = -1;
        } else if (found->Inventory >= 0) {
            const int loaded = found->Clip == 0 ? std::min(clip_size, grant.Rounds) : 0;
            found->Clip += loaded;
            const auto rounds = static_cast<std::int64_t>(found->Inventory) + grant.Rounds - loaded;
            found->Inventory = static_cast<int>(std::min<std::int64_t>(rounds,
                std::max(-1, def->second.MaxInventoryRounds)));
        }
        return true;
    };

    for (const int id : record.WeaponIds) {
        if (grant_weapon({id, record.WeaponRounds}) && soldier.StartingWeapon == 0) {
            soldier.StartingWeapon = id;
        }
    }
    for (const auto &name : record.SpawnPowerups) {
        if (const auto grant = equipment.Powerups.find(name); grant != equipment.Powerups.end()) {
            grant_weapon(grant->second);
        }
    }
    for (const auto &weapon : inventory) {
        if (weapon.Owned) {
            const auto total = static_cast<std::int64_t>(weapon.Clip) + weapon.Inventory;
            soldier.Weapons.push_back({weapon.DefinitionId, weapon.Inventory < 0 ? -1 :
                static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max())),
                equipment.Weapons.at(weapon.DefinitionId).KeyNumber});
        }
    }
    std::stable_sort(soldier.Weapons.begin(), soldier.Weapons.end(), [](const auto &first, const auto &second) {
        return first.KeyNumber < second.KeyNumber;
    });
    if (cPacket::Get_Packet_Header_Size() + SoldierPacketFixedBytes + soldier.Model.size() +
        soldier.Weapons.size() * 2 * sizeof(int) > MAX_BUFFER_SIZE) {
        throw std::runtime_error("Starting inventory is too large for the soldier packet: " + record.Name);
    }
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
    EquipmentDefinitions equipment;
    Load_Presets(definitions, physics, soldiers, equipment);
    if (map_definitions) {
        Load_Presets(*map_definitions, physics, soldiers, equipment);
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
        Load_Starting_Weapons(record, equipment, result.Soldiers[team]);
    }
    return result;
}
