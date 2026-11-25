// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_PLAYER_SESSION_H
#define HEADLESS_PLAYER_SESSION_H

#include <array>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "encodertypeentry.h"
#include "headless_join_data.h"
#include "headless_simulation.h"

class cConnection;
class cPacket;
class LineSegClass;

class HeadlessPlayerSession {
public:
    struct CombatHit {
        int VictimPeer;
        int VictimTeam;
        int ShooterSoldier; // A network ID, which cannot be reused by a new connection.
        double DamagePoints;
        float DeathPoints;
        bool Fatal;
    };

    HeadlessPlayerSession(const HeadlessCollision &collision, cConnection &connection);
    static void Validate_Map(const HeadlessCollision &collision);

    // The joining client must know the soldier's network object before the first update.
    bool Attach_Player(int peer_id, int player_type, int soldier_object_id, const HeadlessSoldierPreset &preset);
    void Introduce_Soldier(int owner_id, int recipient, const HeadlessSoldierPreset &preset);
    int Allocate_Network_Object() { return NextObject--; }
    void Initialize_Buildings();
    void Introduce_Map_Objects(int recipient);
    bool Base_Is_Destroyed(int team) const;
    void End_Combat();
    void Set_Combat_Rules(const HeadlessArmorTable &armor, bool friendly_fire);
    void Set_Player_Loading(int peer_id, bool loading);
    bool Soldier_Is_Dead(int peer_id) const;
    void Remove_Soldier(int peer_id);
    void Remove_Player(int peer_id);
    void Reset_Map(const HeadlessCollision &collision);
    void Receive_Control(cPacket &packet, int peer_id);
    void Advance(HeadlessSimulation::Clock::duration elapsed);
    std::vector<CombatHit> Take_Combat_Hits();
    static bool Client_Owns_Object_Id(int peer_id, int object_id);

private:
    static constexpr float WeaponSwitchSeconds = 1.0f; // WeaponClass::Select uses two half-second switch states.
    enum class WeaponState { Switching, Ready, Charging, Firing, Reloading };

    struct Peer {
        int SoldierObject;
        Vector2 Forward;
        Vector3 Target;
        int ControlObject = 0;
        Vector2 MoveAxes{0, 0};
        HeadlessSimulation::Clock::duration InputRemaining{};
        std::set<int> Viewers;
        bool Ready = true;
        std::vector<HeadlessSpawnWeapon> Weapons;
        int WeaponSlot = 0; // Slot zero is holstered, as in WeaponBagClass.
        ULONG FireControls = 0;
        bool ReloadRequested = false;
        bool DetonateRemoteCharges = false;
        WeaponState WeaponAction = WeaponState::Switching;
        float WeaponTimeRemaining = WeaponSwitchSeconds;
        float Health = 0;
        float Shield = 0;
        unsigned Skin = 0;
        unsigned ShieldType = 0;
        float DamagePoints = 0;
        float DeathPoints = 0;
        float BullseyeHeight = 0;
        bool DefenseChanged = false;
    };

    struct Offense {
        int ShooterObject;
        int Team;
        float Damage;
        unsigned Warhead;
    };

    struct Bullet {
        Offense Attack;
        Vector3 Position;
        Vector3 Direction;
        float Speed;
        float RemainingTravel;
    };

    struct SoldierIntersection {
        int PeerId;
        float Fraction;
    };

    struct Charge {
        int ObjectId;
        int OwnerPeer;
        int OwnerObject;
        int Team;
        HeadlessChargeDefinition Definition;
        Vector3 Position;
        Vector3 Velocity;
        float Facing;
        std::optional<double> FuseRemaining;
        bool Stuck = false;
        std::optional<std::size_t> Building;
        bool StuckMCT = false;
        int AttachedPeer = 0;
        int AttachedObject = 0;
        Vector3 AttachedOffset{0, 0, 0};
    };

    struct BuildingState {
        int ObjectId = 0;
        float Health = 0;
        unsigned Skin = 0;
        unsigned ShieldType = 0;
        int State = -1;
        bool Changed = false;
    };

    bool Read_Control(cPacket &packet, int peer_id, Peer &peer);
    static void Clear_Soldier(Peer &peer);
    static void Select_Weapon(Peer &peer, ULONG controls);
    void Advance_Weapon(int player_id, Peer &peer, float seconds, ULONG fire_controls, bool reload);
    void Fire_Bullet(int player_id, const Peer &peer, const HeadlessBulletDefinition &definition);
    bool Trace_Bullet(const Bullet &bullet, const Vector3 &end);
    std::optional<SoldierIntersection> Cast_Soldiers(const LineSegClass &ray, int ignore_object, float limit) const;
    void Apply_Soldier_Damage(int player_id, Peer &peer, const Offense &attack);
    void Apply_Building_Damage(std::size_t index, bool mct, const Offense &attack);
    void Advance_Bullets(float seconds);
    void Throw_Charge(int owner, const Peer &peer, const HeadlessChargeDefinition &definition, unsigned mode);
    void Advance_Charges(double seconds, float movement_seconds);
    void Move_Charge(Charge &charge, float seconds);
    void Detonate_Charge(const Charge &charge);
    void Send_Charge(const Charge &charge, int recipient, BYTE dirty);
    void Send_Map_Object_Deletion(int object);
    static void Init_Position_Encoders(const HeadlessCollision &collision, std::array<cEncoderTypeEntry, 3> &encoders);
    void Write_Position(cPacket &packet, const Vector3 &position);
    void Write_Defense(cPacket &packet, float health, float shield, unsigned shield_type);
    void Write_Occasional(cPacket &packet, const Peer &peer);
    void Write_Frequent(cPacket &packet, int peer_id, const Peer &peer);
    void Publish_Player(int peer_id, Peer &peer);
    void Send_Building(std::size_t index, int recipient, BYTE dirty);
    void Publish_Buildings();
    void Clear_Buildings();

    HeadlessSimulation Simulation;
    cConnection &Connection;
    // Allocate from the high end of the original dynamic object range.
    int NextObject = 2100000000;
    cEncoderTypeEntry AnalogEncoder;
    std::array<cEncoderTypeEntry, 3> PositionEncoders;
    std::map<int, Peer> Peers;
    const HeadlessArmorTable *Armor = nullptr;
    bool FriendlyFire = false;
    cEncoderTypeEntry VitalsEncoder;
    cEncoderTypeEntry ArmorEncoder;
    std::mt19937 SpreadRandom;
    std::vector<Bullet> Bullets;
    std::vector<Charge> Charges;
    std::vector<CombatHit> Hits;
    std::vector<BuildingState> Buildings;
    std::set<int> MapViewers;
    cEncoderTypeEntry BuildingRadiusEncoder;
    cEncoderTypeEntry BuildingStateEncoder;
    cEncoderTypeEntry ChargeVelocityEncoder;
    HeadlessSimulation::Clock::duration PublishElapsed{};
};

#endif // HEADLESS_PLAYER_SESSION_H
