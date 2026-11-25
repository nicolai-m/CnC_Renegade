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

class HeadlessPlayerSession {
public:
    HeadlessPlayerSession(const HeadlessCollision &collision, cConnection &connection);
    static void Validate_Map(const HeadlessCollision &collision);

    // The joining client must know the soldier's network object before the first update.
    bool Attach_Player(int peer_id, int player_type, int soldier_object_id, const HeadlessSoldierPreset &preset);
    void Introduce_Soldier(int owner_id, int recipient, const HeadlessSoldierPreset &preset);
    void Set_Combat_Rules(const HeadlessArmorTable &armor, bool friendly_fire);
    void Set_Player_Loading(int peer_id, bool loading);
    void Remove_Player(int peer_id);
    void Reset_Map(const HeadlessCollision &collision);
    void Receive_Control(cPacket &packet, int peer_id);
    void Advance(HeadlessSimulation::Clock::duration elapsed);
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
        WeaponState WeaponAction = WeaponState::Switching;
        float WeaponTimeRemaining = WeaponSwitchSeconds;
        float Health = 0;
        float Shield = 0;
        unsigned Skin = 0;
        unsigned ShieldType = 0;
        bool DefenseChanged = false;
    };

    struct Bullet {
        int ShooterObject;
        int Team;
        HeadlessBulletDefinition Definition;
        Vector3 Position;
        Vector3 Direction;
        float RemainingTravel;
    };

    bool Read_Control(cPacket &packet, int peer_id, Peer &peer);
    static void Select_Weapon(Peer &peer, ULONG controls);
    void Advance_Weapon(int player_id, Peer &peer, float seconds, ULONG fire_controls, bool reload);
    void Fire_Bullet(int player_id, const Peer &peer, const HeadlessBulletDefinition &definition);
    bool Trace_Bullet(const Bullet &bullet, const Vector3 &end);
    void Apply_Bullet_Damage(int player_id, Peer &peer, const HeadlessBulletDefinition &bullet);
    void Advance_Bullets(float seconds);
    static void Init_Position_Encoders(const HeadlessCollision &collision, std::array<cEncoderTypeEntry, 3> &encoders);
    void Write_Position(cPacket &packet, const Vector3 &position);
    void Write_Occasional(cPacket &packet, const Peer &peer);
    void Write_Frequent(cPacket &packet, int peer_id, const Peer &peer);
    void Publish_Player(int peer_id, Peer &peer);

    HeadlessSimulation Simulation;
    cConnection &Connection;
    cEncoderTypeEntry AnalogEncoder;
    std::array<cEncoderTypeEntry, 3> PositionEncoders;
    std::map<int, Peer> Peers;
    const HeadlessArmorTable *Armor = nullptr;
    bool FriendlyFire = false;
    cEncoderTypeEntry VitalsEncoder;
    cEncoderTypeEntry ArmorEncoder;
    std::mt19937 SpreadRandom;
    std::vector<Bullet> Bullets;
    HeadlessSimulation::Clock::duration PublishElapsed{};
};

#endif // HEADLESS_PLAYER_SESSION_H
