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
#include "wwphysids.h"

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
constexpr uint32 AmmoVariables = 1206091429;
constexpr uint32 AmmoParent = 1206091430;
constexpr uint32 TossedChargeParent = 930991700;
constexpr uint32 TossedChargeVariables = TossedChargeParent + 1;
constexpr uint32 MoveablePhysicsVariables = 0x04486001;
constexpr uint32 ExplosionVariables = 0317001525;
constexpr uint32 ExplosionParent = ExplosionVariables + 1;
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
    int SkinSaveId = 0;
    int ShieldSaveId = 0;
    std::vector<std::string> SpawnPowerups;
    float ThrowSpeed = 5;
    float GravityScale = 1;
};

struct WeaponDefinition {
    int Style = 1;
    float KeyNumber = 0;
    int ClipSize = 0;
    int MaxInventoryRounds = 100;
    float ReloadSeconds = 0;
    int PrimaryAmmo = 0;
    int SecondaryAmmo = 0;
    bool CanSnipe = false;
};

struct AmmoDefinition {
    int Type = 0;
    int BurstMax = 0;
    int RoundCost = 1;
    float RateOfFire = 1;
    float ChargeSeconds = 0;
    int Warhead = 0;
    float Damage = 1;
    float Range = 10;
    float Speed = 1;
    float Gravity = 0;
    float Spread = 0;
    int SprayCount = 1;
    int SoftPierceLimit = 0;
    bool Tracking = false;
    int Explosion = 0;
    std::string Model;
    std::array<float, 3> FuseSeconds{};
};

struct WeaponGrant {
    int DefinitionId = 0;
    int Rounds = 0;
    bool GiveWeapon = true;
};

struct EquipmentDefinitions {
    std::map<int, WeaponDefinition> Weapons;
    std::map<int, AmmoDefinition> Ammo;
    std::map<std::string, WeaponGrant> Powerups;
    std::map<int, HeadlessExplosionDefinition> Explosions;
    std::map<int, float> ProjectileGravity;
    Record TossedCharge;
};

std::string Model_Name(const std::string &path)
{
    const auto basename = path.substr(path.find_last_of("/\\") + 1);
    return basename.substr(0, basename.find_last_of('.'));
}

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

float Read_Defense_Value(BoundedChunkReader &reader)
{
    const float value = reader.Value<float>();
    reader.Require(std::isfinite(value) && value >= 0 && value <= MaxDefense, "Invalid soldier defense value");
    return value;
}

void Read_Record(BoundedChunkReader &reader, Record &record, uint32 parent, unsigned depth = 0)
{
    reader.Require(depth < 20, "Game object definition is nested too deeply");
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
        } else if (id == PhysicalVariables && (parent == ArmedPhysicalParent || parent == TossedChargeParent)) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 18) {
                    record.PhysicsId = reader.Value<sint32>();
                } else if (field == 2) {
                    const auto height = reader.Value<float>();
                    reader.Require(std::isfinite(height), "Invalid game object bullseye offset");
                    record.Soldier.BullseyeHeight = height;
                }
            });
        } else if (id == DefenseVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 0) {
                    record.Soldier.Health = Read_Defense_Value(reader);
                } else if (field == 2) {
                    record.SkinSaveId = reader.Value<sint32>();
                } else if (field == 3) {
                    record.Soldier.Shield = Read_Defense_Value(reader);
                } else if (field == 5) {
                    record.ShieldSaveId = reader.Value<sint32>();
                } else if (field == 6 || field == 7) {
                    const float points = reader.Value<float>();
                    reader.Require(std::isfinite(points), "Invalid soldier score coefficient");
                    if (field == 6) {
                        record.Soldier.DamagePoints = points;
                    } else {
                        record.Soldier.DeathPoints = points;
                    }
                }
            });
        } else if (id == PhysicsModelVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 1) {
                    record.Soldier.Model = Model_Name(reader.Read_String());
                }
            });
        } else if (id == TossedChargeVariables || id == MoveablePhysicsVariables) {
            reader.Each_Micro([&](uint32 field) {
                if (field == 1) {
                    auto &value = id == TossedChargeVariables ? record.ThrowSpeed : record.GravityScale;
                    value = reader.Value<float>();
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
                if (field == 1) {
                    weapon.Style = reader.Value<sint32>();
                } else if (field == 9) {
                    weapon.ReloadSeconds = reader.Value<float>();
                } else if (field == 16) {
                    weapon.CanSnipe = reader.Value<uint8>() != 0;
                } else if (field == 27) {
                    weapon.PrimaryAmmo = reader.Value<sint32>();
                } else if (field == 28) {
                    weapon.SecondaryAmmo = reader.Value<sint32>();
                } else if (field == 29) {
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

void Read_Ammo(BoundedChunkReader &reader, EquipmentDefinitions &equipment)
{
    int definition_id = 0;
    std::string name;
    AmmoDefinition ammo;
    reader.Each_Chunk([&](uint32 id) {
        if (id == AmmoParent) {
            reader.In_Chunk(DefinitionIdentity, [&] { Read_Identity(reader, definition_id, name); });
        } else if (id == AmmoVariables) {
            reader.Each_Micro([&](uint32 field) {
                switch (field) {
                case 2: ammo.Model = Model_Name(reader.Read_String()); break;
                case 3: ammo.Warhead = reader.Value<sint32>(); break;
                case 4: ammo.Damage = reader.Value<float>(); break;
                case 5: ammo.Range = reader.Value<float>(); break;
                case 6: ammo.Speed = reader.Value<float>(); break;
                case 7: ammo.Gravity = reader.Value<float>(); break;
                case 10: ammo.Spread = reader.Value<float>(); break;
                case 11: ammo.SprayCount = reader.Value<sint32>(); break;
                case 15: ammo.BurstMax = reader.Value<sint32>(); break;
                case 17: ammo.SoftPierceLimit = reader.Value<sint32>(); break;
                case 21: ammo.Tracking = reader.Value<uint8>() != 0; break;
                case 24: ammo.Explosion = reader.Value<sint32>(); break;
                case 27: ammo.ChargeSeconds = reader.Value<float>(); break;
                case 31: ammo.RoundCost = reader.Value<sint32>(); break;
                case 32: ammo.Type = reader.Value<sint32>(); break;
                case 35:
                case 36:
                case 37: ammo.FuseSeconds[field - 35] = reader.Value<float>(); break;
                case 48: ammo.RateOfFire = reader.Value<float>(); break;
                }
            });
        }
    });
    if (definition_id > 0) {
        equipment.Ammo[definition_id] = std::move(ammo);
    }
}

void Read_Explosion(BoundedChunkReader &reader, EquipmentDefinitions &equipment)
{
    HeadlessExplosionDefinition explosion;
    reader.Each_Chunk([&](uint32 id) {
        if (id == ExplosionParent) {
            std::string name;
            reader.In_Chunk(DefinitionIdentity, [&] { Read_Identity(reader, explosion.Id, name); });
        } else if (id == ExplosionVariables) {
            reader.Each_Micro([&](uint32 field) {
                switch (field) {
                case 3: explosion.Radius = reader.Value<float>(); break;
                case 4: explosion.Damage = reader.Value<float>(); break;
                case 5: explosion.Warhead = reader.Value<uint32>(); break;
                case 6: explosion.Scaled = reader.Value<uint8>() != 0; break;
                }
            });
        }
    });
    if (explosion.Id > 0) {
        equipment.Explosions[explosion.Id] = explosion;
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
    EquipmentDefinitions &equipment, HeadlessSpawnCharacter character)
{
    const bool engineer = character == HeadlessSpawnCharacter::Engineer;
    BoundedChunkReader reader(source);
    reader.In_Chunk(CHUNKID_SAVELOAD_DEFMGR, [&] {
        reader.In_Chunk(DefinitionObjects, [&] {
            reader.Each_Chunk([&](uint32 factory) {
                if (factory == CHUNKID_WEAPON_DEF) {
                    reader.In_Chunk(FactoryData, [&] { Read_Weapon(reader, equipment); });
                    return;
                }
                if (factory == CHUNKID_AMMO_DEF) {
                    reader.In_Chunk(FactoryData, [&] { Read_Ammo(reader, equipment); });
                    return;
                }
                if (factory == CHUNKID_EXPLOSION_DEF) {
                    reader.In_Chunk(FactoryData, [&] { Read_Explosion(reader, equipment); });
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
                if (factory != CHUNKID_GAME_OBJECT_DEF_SOLDIER && factory != HumanPhysicsFactory &&
                    factory != CHUNKID_GAME_OBJECT_DEF_C4 && factory != PHYSICS_CHUNKID_PROJECTILEDEF) {
                    return;
                }
                Record record;
                reader.In_Chunk(FactoryData, [&] { Read_Record(reader, record, FactoryData); });
                if (factory == HumanPhysicsFactory) {
                    physics[record.Soldier.DefinitionId] = std::move(record.Soldier.Model);
                } else if (factory == PHYSICS_CHUNKID_PROJECTILEDEF) {
                    equipment.ProjectileGravity[record.Soldier.DefinitionId] = record.GravityScale;
                } else if (factory == CHUNKID_GAME_OBJECT_DEF_C4) {
                    if (Fold_Ascii(record.Name) == "tossed c4") {
                        equipment.TossedCharge = std::move(record);
                    }
                } else {
                    const auto name = Fold_Ascii(record.Name);
                    if (name == (engineer ? "cnc_nod_engineer_0" : "cnc_nod_minigunner_0")) {
                        soldiers[PLAYERTYPE_NOD] = std::move(record);
                    } else if (name == (engineer ? "cnc_gdi_engineer_0" : "cnc_gdi_minigunner_0")) {
                        soldiers[PLAYERTYPE_GDI] = std::move(record);
                    }
                }
            });
        });
    });
}

std::optional<HeadlessFireMode> Get_Fire_Mode(const EquipmentDefinitions &equipment, const HeadlessArmorTable &armor,
    int id, float charge_seconds, bool c4)
{
    constexpr int NormalAmmo = 0;
    constexpr int RemoteC4Ammo = 1;
    constexpr int TimedC4Ammo = 2;
    constexpr float MaxShotsPerSecond = 1000;
    const auto def = equipment.Ammo.find(id);
    if (def == equipment.Ammo.end()) {
        return std::nullopt;
    }
    const auto &ammo = def->second;
    if ((c4 ? ammo.Type != RemoteC4Ammo && ammo.Type != TimedC4Ammo : ammo.Type != NormalAmmo) ||
        ammo.BurstMax != 0 || ammo.RoundCost < 0 ||
        !std::isfinite(ammo.RateOfFire) || ammo.RateOfFire <= 0 || ammo.RateOfFire > MaxShotsPerSecond ||
        !std::isfinite(charge_seconds) || charge_seconds < 0) {
        return std::nullopt;
    }
    const float interval = 1.0f / ammo.RateOfFire;
    if (!std::isfinite(interval)) {
        return std::nullopt;
    }
    HeadlessFireMode mode{interval, charge_seconds, ammo.RoundCost, {}};
    if (c4) {
        const bool timed = ammo.Type == TimedC4Ammo;
        const auto &tossed = equipment.TossedCharge;
        const auto projectile = equipment.ProjectileGravity.find(tossed.PhysicsId);
        const auto explosion = equipment.Explosions.find(ammo.Explosion);
        if (tossed.Soldier.DefinitionId <= 0 || projectile == equipment.ProjectileGravity.end() ||
            explosion == equipment.Explosions.end() || ammo.Model.empty() || ammo.Model.size() > 255 || ammo.RoundCost <= 0 ||
            !std::isfinite(tossed.ThrowSpeed) || tossed.ThrowSpeed <= 0 || tossed.ThrowSpeed > 90 ||
            !std::isfinite(projectile->second) || projectile->second < 0 ||
            (timed && std::any_of(ammo.FuseSeconds.begin(), ammo.FuseSeconds.end(), [](float seconds) {
                return !std::isfinite(seconds) || seconds <= 0;
            }))) {
            return std::nullopt;
        }
        const auto &blast = explosion->second;
        if (!std::isfinite(blast.Radius) || blast.Radius < 0 || !std::isfinite(blast.Damage) || blast.Damage <= 0 ||
            armor.empty() || blast.Warhead >= armor.front().Warheads.size()) {
            return std::nullopt;
        }
        mode.Charge = HeadlessChargeDefinition{timed ? HeadlessChargeTrigger::Timed : HeadlessChargeTrigger::Remote,
            tossed.Soldier.DefinitionId, id, ammo.Model, tossed.ThrowSpeed, projectile->second,
            ammo.FuseSeconds, tossed.Soldier.Health, tossed.Soldier.Shield,
            Headless_Armor_Index(armor, tossed.ShieldSaveId), blast};
        return mode;
    }
    // The minigunner uses single, straight bullets. The other projectile behaviors need their own simulation.
    if (ammo.Warhead >= 0 && std::isfinite(ammo.Damage) && ammo.Damage > 0 &&
        std::isfinite(ammo.Range) && ammo.Range > 0 && ammo.Range <= 1000 &&
        std::isfinite(ammo.Speed) && ammo.Speed > 0 &&
        std::isfinite(ammo.Spread) && ammo.Spread >= 0 && ammo.Spread <= 1 &&
        ammo.Gravity == 0 && ammo.SprayCount <= 1 && ammo.SoftPierceLimit <= 1 && !ammo.Tracking && !ammo.Explosion) {
        mode.Bullet = HeadlessBulletDefinition{ammo.Damage, static_cast<unsigned>(ammo.Warhead),
            ammo.Range, ammo.Speed, ammo.Spread};
    }
    return mode;
}

void Load_Starting_Weapons(const Record &record, const EquipmentDefinitions &equipment, const HeadlessArmorTable &armor,
    HeadlessSoldierPreset &soldier)
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
            const auto &def = equipment.Weapons.at(weapon.DefinitionId);
            const float reload = std::isfinite(def.ReloadSeconds) ? std::max(0.0f, def.ReloadSeconds) : 0.0f;
            HeadlessSpawnWeapon spawn{weapon.DefinitionId, def.KeyNumber, std::max(0, def.ClipSize),
                weapon.Clip, weapon.Inventory, reload, {}, {}};
            constexpr int RifleStyle = 2;
            constexpr int HandgunStyle = 5;
            constexpr int C4Style = 0;
            const bool c4 = def.Style == C4Style;
            if (spawn.ClipSize > 0 && (def.Style == RifleStyle || def.Style == HandgunStyle || c4)) {
                const auto ammo = equipment.Ammo.find(def.PrimaryAmmo);
                if (ammo != equipment.Ammo.end()) {
                    const float charge = ammo->second.ChargeSeconds; // WeaponClass uses primary charge for both triggers.
                    spawn.PrimaryFire = Get_Fire_Mode(equipment, armor, def.PrimaryAmmo, charge, c4);
                    // C4 uses secondary input for the one-shot weapon-use action.
                    if (spawn.PrimaryFire && !def.CanSnipe && !c4) {
                        spawn.SecondaryFire = Get_Fire_Mode(equipment, armor, def.SecondaryAmmo, charge, false);
                    }
                }
            }
            soldier.Weapons.push_back(spawn);
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

int HeadlessSpawnWeapon::Total_Rounds() const
{
    // WeaponBagClass and SoldierGameObj both send -1 for an unlimited reserve.
    const auto total = static_cast<std::int64_t>(ClipRounds) + ReserveRounds;
    return ReserveRounds < 0 ? -1 : static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max()));
}

unsigned Headless_Armor_Index(const HeadlessArmorTable &armor, int save_id)
{
    const auto found = std::find_if(armor.begin(), armor.end(), [=](const auto &type) { return type.SaveId == save_id; });
    return found == armor.end() ? 0 : static_cast<unsigned>(found - armor.begin());
}

HeadlessJoinData Load_Headless_Join_Data(uint32 client_build, const HeadlessArmorTable &armor,
    const GameDataFile &strings, const GameDataFile &definitions, const std::optional<GameDataFile> &map_definitions,
    const HeadlessCollision::FindAsset &find_core_asset, HeadlessSpawnCharacter character)
{
    HeadlessJoinData result;
    const auto exe = "RENEGADE " + std::to_string(client_build);
    const auto tdb = "strings.tdb " + std::to_string(Read_Strings_Version(strings));
    result.ExeCRC = Build_CRC(exe);
    result.StringsCRC = Build_CRC(tdb);
    result.VersionKey = Build_CRC(exe + " " + tdb + " ") ^ Data_CRC(find_core_asset);
    result.Armor = armor;

    std::array<Record, 2> soldiers;
    std::map<int, std::string> physics;
    EquipmentDefinitions equipment;
    Load_Presets(definitions, physics, soldiers, equipment, character);
    if (map_definitions) {
        Load_Presets(*map_definitions, physics, soldiers, equipment, character);
    }
    for (std::size_t team = 0; team < soldiers.size(); ++team) {
        const auto &record = soldiers[team];
        const auto model = physics.find(record.PhysicsId);
        if (record.Soldier.DefinitionId <= 0 || record.Soldier.Health <= 0 ||
            armor.empty() ||
            model == physics.end() || model->second.empty()) {
            throw std::runtime_error(character == HeadlessSpawnCharacter::Engineer ?
                "Missing or invalid Nod/GDI engineer preset" : "Missing or invalid Nod/GDI minigunner preset");
        }
        result.Soldiers[team] = record.Soldier;
        result.Soldiers[team].Model = model->second;
        result.Soldiers[team].Skin = Headless_Armor_Index(armor, record.SkinSaveId);
        result.Soldiers[team].ShieldType = Headless_Armor_Index(armor, record.ShieldSaveId);
        Load_Starting_Weapons(record, equipment, armor, result.Soldiers[team]);
    }
    return result;
}
