
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

The bootstrap loads the first configured map into a headless scene using the
original chunk reader. It restores the map bounds and spatial tree, static object
model references and transforms, building state, and spawner placements with
their team definitions. It also checks the other maps’ chunk headers. With
`--check` it exits after loading and validation. Otherwise it listens for direct
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
The bootstrap refuses joins while player and vehicle movement, animated and
dynamic objects, the simulation loop, and game-session callbacks are being
ported. Tests exercise scene loading, collision masks and hit results, the UDP
handshake, reliable delivery, and bootstrap startup and shutdown.


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
