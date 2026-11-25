
# Command & Conquer Renegade

This repository includes source code for Command & Conquer Renegade. This release provides support to the [Steam Workshop](https://steamcommunity.com/workshop/browse/?appid=2229890) for the game.


## Dependencies

If you wish to rebuild the source code and tools successfully you will need to find or write new replacements (or remove the code using them entirely) for the following libraries;

- DirectX SDK (Version 8.0 or higher) (expected path `\Code\DirectX\`)
- RAD Bink SDK - (expected path `\Code\BinkMovie\`)
- RAD Miles Sound System SDK - (expected path `\Code\Miles6\`)
- NvDXTLib SDK - (expected path `\Code\NvDXTLib\`)
- Lightscape SDK - (expected path `\Code\Lightscape\`)
- Umbra SDK - (expected path `\Code\Umbra\`)
- GameSpy SDK - (expected path `\Code\GameSpy\`)
- GNU Regex - (expected path `\Code\WWLib\`)
- SafeDisk API - (expected path `\Code\Launcher\SafeDisk\`)
- Microsoft Cab Archive Library - (expected path `\Code\Installer\Cab\`)
- RTPatch Library - (expected path `\Code\Installer\`)
- Java Runtime Headers - (expected path `\Code\Tools\RenegadeGR\`)


## Building

To use the compiled binaries, you must own the game. The C&C Ultimate Collection is available for purchase on [EA App](https://www.ea.com/en-gb/games/command-and-conquer/command-and-conquer-the-ultimate-collection/buy/pc) or [Steam](https://store.steampowered.com/bundle/39394/Command__Conquer_The_Ultimate_Collection/).

### Renegade (Win32)

The quickest way to build all configurations in the project is to open `commando.dsw` in Microsoft Visual Studio C++ 6.0 (SP5 recommended for binary matching to patch 1.037) and select Build -> Batch Build, then hit the “Rebuild All” button.

If you wish to compile the code under a modern version of Microsoft Visual Studio, you can convert the legacy project file to a modern MSVC solution by opening the `commando.dsw` in Microsoft Visual Studio .NET 2003, and then opening the newly created project and solution file in MSVC 2015 or newer.

NOTE: As modern versions of MSVC enforce newer revisions of the C++ standard, you will need to make extensive changes to the codebase before it successfully compiles, even more so if you plan on compiling for the Win64 platform.

When the workspace has finished building, the compiled binaries will be copied to the `/Run/` directory found in the root of this repository. 


### Free Dedicated Server
It’s possible to build the Windows version of the FDS (Free Dedicated Server) for Command & Conquer Renegade from the source code in this repository, just uncomment `#define FREEDEDICATEDSERVER` in [Combat\specialbuilds.h](Combat\specialbuilds.h) and perform a “Rebuild All” action on the Release config.

### Linux Dedicated Server port (in progress)

Build the native `renegade_fds` bootstrap on amd64 or arm64 with CMake 3.20 or
newer and a C++17 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use a locally installed copy of the game data. `--data-dir` accepts the game
directory or its `Data` directory. `--config` accepts an existing `[Settings]`
INI such as `svrcfg_cnc.ini`, or a `[Server]` INI with `GameType=LAN` and
`Config=<filename>` referring to a settings INI in the data directory.

```sh
./build/renegade_fds --data-dir /path/to/game --config /path/to/svrcfg_cnc.ini --check
./build/renegade_fds --data-dir /path/to/game --config /path/to/svrcfg_cnc.ini
```

The bootstrap loads the configured maps into headless scenes using the original
chunk reader. It restores map bounds and spatial trees, static object model
references and transforms, building state, and spawner placements with their
team definitions. With `--check` it exits after loading and validation.
Otherwise it listens for direct
UDP on `127.0.0.1` by default, with port `4848` if the INI does not specify one.
`--bind` and `--port` override the address and port. `SIGINT` and `SIGTERM` stop
it cleanly.

The server uses the spatial tree for rays and swept boxes against static W3D
triangles and collision boxes, including hierarchical models in their base pose.
Box sweeps take an axis-aligned or oriented hull and test translation at a fixed
orientation. The result includes the first contact or an initial overlap.
Startup reports both ground-ray and box-sweep results at enabled soldier starts.
The box probe uses the soldier debug box dimensions with its feet 0.25 m above
the saved position.
The server advances a headless simulation every 20 ms. It can place a soldier on
an enabled team spawn with walkable ground below it and apply movement input.
Soldiers use the debug-sized collision box, a fixed movement speed of 4 m/s,
gravity, and sliding against static geometry. With `--check`, the server also
reports whether it can place a grounded soldier for each team.

Add `ClientBuildNumber` to the server's `[Settings]` to enable experimental
direct UDP joins. Use the build number shown by the client and matching game
data. The server computes the original version key from the build number,
`strings.tdb`, and the compatibility file list. With `--check`, it prints the
computed key. Without `ClientBuildNumber`, the bootstrap refuses joins.

With joins enabled, the server announces the game on UDP port `3373` once per
second so it appears in the original client's LAN list. Use `--bind` with the
server's LAN IPv4 address, or `0.0.0.0` to announce on all broadcast-capable
IPv4 interfaces. The default loopback binding stays local. To send the
announcement to a specific broadcast or unicast address, use
`--lan-destination IPV4`. Give each server a distinct `[Server] Nickname` when
running multiple servers on the same LAN.

The join sends the original teams and game options and waits for the client's
post-load biography before creating its player and soldier objects. Connected
peers receive position and movement updates for the soldiers known to them.

Soldiers start with the weapons and ammunition in their original presets
and the weapon grants from `M00_GrantPowerup_Created`. The server sends the
inventory on spawn and synchronizes weapon selection, including number keys,
cycling, and holstering. For the stock rifles and pistol, the server tracks
magazine and reserve rounds, uses the original firing rates and reload times,
and synchronizes trigger state and remaining ammunition. Both manual and
automatic reloads use the weapon's reserve.

The server traces the stock minigunner's rifle and pistol shots against static
map geometry and soldier collision boxes. Rifles use projectile travel time.
The pistol resolves the hit immediately, as in the original game. Shield
absorption and skin multipliers come from `armor.ini`. At zero health the
server stops accepting the soldier's inputs. After two seconds it deletes
the old soldier and respawns the configured soldier class with the same team and fresh
health, shield, and starting inventory. The player and round stay active.
Set `IsFriendlyFirePermitted=yes` in `[Settings]` to enable damage to allies
and their buildings. It defaults to off. Skeletal hit zones and vehicle damage
remain open.

The server records kills and deaths on the player and team objects and sends
the original kill notification to players in the match. A respawn preserves the
counters. The next round resets them. Friendly-fire kills count when enabled,
as on the Windows server.

Minigunner hits award the damage points and kill bonus defined on the victim's
preset. Shield damage counts in unscaled units, as in the Windows damage
calculation. Points apply to the player and team. Positive awards also grant
credits to the player, and friendly fire deducts points.

Rifle and pistol hits also damage buildings. The server associates static map
components with their buildings and applies the armor defined for the target
component, including the alternate armor of the Master Control Terminal.
Clients receive the original building health and destruction updates.
Building damage and destruction award preset points and credits.

Timed, remote, and proximity C4 use the original weapon, projectile, and explosion
presets. A throw consumes one charge. The charge sticks
to terrain, a building component, or a soldier's collision box. Detonation
damages nearby soldiers, with reduced damage through cover. A charge stuck to
a building or its Master Control Terminal damages that building according to
the armor of the component it hit. Clients receive the charge and the original
explosion event. A round transition removes any remaining charges.

Set `SpawnCharacter=Engineer` in `[Settings]` to spawn both teams with
their original engineer loadouts, including two remote charges. The default
`SpawnCharacter=Minigunner` uses the original minigunner loadouts. Select
remote C4 and use primary fire to throw a charge. With C4 equipped, secondary
fire detonates all remote charges belonging to that soldier. Remote charges
have no fuse and are disarmed when the owner disconnects or their soldier
is removed.

Set `SpawnCharacter=Technician` to use the original Nod Technician and GDI
Hotwire loadouts. Both start with five proximity mines. Select proximity C4
and use primary fire to throw a mine. It uses the arming delay and trigger
range in its ammo preset. The stock mine arms immediately and checks for
opposing soldiers within three meters every quarter second. Remote charges
and proximity mines share a limit of 30 per team. A new placement at that
limit disarms the team's oldest mine or remote charge. Purchase terminals,
vehicle-triggered proximity detection, vehicle repair, and animated
attachments still need to be ported.

Engineers and technicians can use their original unlimited-ammunition repair
gun. The server applies the armor rules in `armor.ini` to its beam, including
the alternate armor of the Master Control Terminal. The original
`CanRepairBuildings=no` setting disables building repair in `[Settings]`.
With repair enabled, the beam restores standing buildings up to their
original maximum health. Repairs to allies award half the damage points for
the amount restored. A beam can also disarm enemy C4 and proximity mines.
The server sends the original harmless disarm effect and removes the charge.

Linux loads the ground vehicles and stationary turrets placed directly in a
map's saved scene and sends them to clients. Use the action key at a ground
vehicle's original entry zone to take the driver seat. Use it again to leave
at an unobstructed exit. A later client receives the occupied seat, and the
server clears it if the driver disconnects. Vehicles stay parked in this
version. Driving, passenger seats, vehicle weapons, and AI turret behavior
still need to be ported.

`TimeLimitMinutes` starts counting down when the server starts the loaded map.
Clients receive the remaining round time when joining and after loading the
map. A value of `0` disables the time limit. When the timer expires, the server
sends the original results event with the next map and advertises that map for
new joins. After a ten-second intermission it starts the next round over the
existing connections and spawns players as each client finishes loading.
`MapName` selects the initial map. Subsequent maps come from `MapName00` through
`MapName99` in order, with a matching `MapName00` counted once. The default
`DoMapsLoop=yes` returns to the first map after the last. With `DoMapsLoop=no`,
the server withdraws its LAN entry and exits after the final intermission.
At the time limit the team with the highest score wins, with kills breaking a
points tie. An exact tie goes to Nod. Destroying the last building on an
opposing team wins the round and awards the original base destruction bonus.
Set `BaseDestructionEndsGame=no` in `[Settings]` to keep playing after a base is
destroyed. A new round clears scores and credits and restores the buildings.
Beacon victory and building functions such as power and production are still
being ported.

The two-client UDP fixture covers login, map readiness, movement replication,
the remaining time for a late join, spawn inventory, weapon selection visible
to both peers, firing and reloads, cover, projectile travel, shield and health
damage, blocked input at zero health, respawn with the original player and
control objects, combat points, credits and kills, object removal, and LAN
announcements and withdrawal.
The round fixture advances the server clock through a timeout
and checks the timed result and tiebreakers, the announced map, its spawn and
position encoding, movement after a map reload, and the end of the map cycle.
The building fixture checks exterior and terminal armor, replicated damage
and destruction, points and credits, both settings for the base destruction
rule, and restored buildings in the next round. The timed C4 case verifies
ammunition usage, attachment to a terminal, replication for a late join,
the selected fuse, and a base destruction win after detonation.
The remote C4 case covers the engineer loadout, both charges surviving
until their owner triggers them, visibility for a late join, and disarming
when the owner disconnects.
The proximity fixture verifies the technician's ammunition grant, arming
delay, selected trigger radius, late-join replication, and a single detonation
when the opposing soldier moves into range.
The repair fixture verifies the unlimited clip, capped repairs, scoring,
the building repair setting, and disarming remote C4 and proximity mines.
The vehicle fixture checks the legacy save pointer and packet layout, entry
range, exclusive driver occupancy, late joins, exit placement, disconnects,
and vehicle removal on map reset.

On Linux arm64 and with the Linux amd64 build running under QEMU, graphical EA
1.038 Windows clients loaded `C&C_Field.mix`, spawned and displayed another
player's movement. These tests used a separate client copy with
`C&C_Field.mix` as the only `.mix` in `Data`.

The tested EA 1.038 Windows executable (February 2025 build) computes the map
lookup CRC from the last file returned by the Windows `Data\*.mix` search on
every comparison. With all 26 installed maps, the LAN browser showed the
server running `Skirmish00.mix`, the last file returned, and omitted the
server running `C&C_Field.mix`. Both amd64 servers sent LAN announcements
under QEMU, and a direct connection to the `Skirmish00.mix` server loaded the
map and spawned the player. For testing other maps with this particular
client, use a separate client copy with only the target `.mix` in `Data`.
For the graphical rotation test, a separate copy registered the second map's
archive at startup under a filename sorting before the first map. The test
made each target map name the final `Data\*.mix` search result just before
its transition. The unchanged client switched from `C&C_Field.mix` to
`C&C_Under.mix` and back over the same connection.

Graphical testing on native Linux amd64, multiple-map testing with a client
that handles the full map set, combat, and match rules are still pending.


### Level Edit (Public Release)
To build the public release build of Level Edit, modify the LevelEdit project settings and add `PUBLIC_EDITOR_VER` to the preprocessor defines.


## Known Issues

The “Debug” configuration of the “Commando” project (the Renegade main project) will sometimes fail to link the final executable. This is due to Windows Defender incorrectly detecting RenegadeD.exe containing a virus (possibly due to the embedded browser code). Excluding the output `/Run/` folder found in the root of this repository in Windows Defender should resolve this for you.


## Contributing

This repository will not be accepting contributions (pull requests, issues, etc). If you wish to create changes to the source code and encourage collaboration, please create a fork of the repository under your GitHub user/organization space.


## Support

This repository is for preservation purposes only and is archived without support. 


## License

This repository and its contents are licensed under the GPL v3 license, with additional terms applied. Please see [LICENSE.md](LICENSE.md) for details.
