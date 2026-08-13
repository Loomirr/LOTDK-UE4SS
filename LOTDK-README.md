# Loomirr's UE4SS for LEGO Batman: Legacy of the Dark Knight

This repository is a game-specific fork of [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) for **LEGO Batman: Legacy of the Dark Knight** (LOTDK). It provides a complete UE4SS installation configured and tested for LOTDK while retaining the upstream Lua scripting, C++ modding, Live View, object-dumping, and Blueprint-mod capabilities.

This is a community compatibility project. It is not an official RE-UE4SS release and is not affiliated with The LEGO Group, DC, Warner Bros. Games, or the game's developers and publishers.

## Source baseline

- Upstream project: [UE4SS-RE/RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
- Pinned upstream commit: `b872ad111394cfabda302dbe744f6893126dfd87`
- Upstream license: MIT
- LOTDK build profile: `Game__Shipping__Win64`
- Tested LOTDK executable version: `1312910`

The original upstream [`README.md`](README.md) documents the complete UE4SS feature set, API, build requirements, and general architecture. This file documents only the LOTDK-specific distribution and maintained changes.

## LOTDK-specific changes

This fork adds and maintains:

- A LOTDK-compatible `dwmapi.dll` proxy/bootstrap implementation.
- Early discovery of `.uplugin` files under `Binaries\Win64\ue4ss\LOTDKExpanded\RegistryPlugins` so LOTDK Expanded can register native mod assets before game startup.
- A `--disable-ue4ss` recovery path that bypasses both UE4SS and LOTDK registry-plugin bootstrapping.
- A tested UE 5.6 LOTDK configuration profile.
- LOTDK-specific `ConsoleManager`, `StaticConstructObject`, and `GUObjectHashTables` signatures.
- A stable external UE4SS Debugging Tools window with Live View search and individual UObject dumps enabled for LOTDK.
- A LOTDK-safe Unreal command field that dispatches reflected `KismetSystemLibrary.ExecuteConsoleCommand` calls on the ProcessEvent game thread.
- A single `~` hotkey for opening and closing Debugging Tools.
- Validation and repair of missing, corrupt, tiny, or off-screen saved Debugging Tools window geometry.
- Release packaging, validation, checksums, and documentation for a self-contained player installation.

The LOTDK Expanded bootstrap is inert when LOTDK Expanded is not installed. LOTDK Expanded, Batcomputer, LOTDK Mod Manager, custom suits, and other gameplay mods are separate projects and are not included in this source repository.

## Verified functionality

The v0.1.0 player package has been tested for:

- Normal startup, front-end navigation, and gameplay.
- Standard UE4SS Lua-mod loading.
- Lua keybind registration and execution.
- UE4SS C++-mod loading.
- External Debugging Tools startup and window restoration.
- Live View search and individual object dumps.
- Reflected Unreal console commands such as `stat fps`.
- LOTDK Expanded loading and registry-plugin discovery.
- Native custom-suit discovery, selection, startup restoration, and gameplay.
- Recovery startup using `--disable-ue4ss`.

Some optional UE4SS lifecycle hooks, including `LoadMap`, `InitGameState`, `BeginPlay`, and `EndPlay`, are unavailable in the current LOTDK compatibility profile. Ordinary Lua loading and keybinds are verified, but a Lua mod that specifically requires one of those hooks may need a LOTDK-specific adaptation.

## Player installation

Player-ready binaries are distributed through this repository's [GitHub Releases](https://github.com/Loomirr/LOTDK-UE4SS/releases) and the corresponding Nexus Mods page. Do not use GitHub's automatically generated source archive as the player installation.

For a clean installation:

1. Close the game.
2. Open the game's `LEGOBatmanLotDK` directory.
3. Back up or rename any existing `Binaries\Win64\ue4ss` folder and `Binaries\Win64\dwmapi.dll` file.
4. Extract the player ZIP directly into `LEGOBatmanLotDK`.
5. Confirm that `LEGOBatmanLotDK\Binaries\Win64\dwmapi.dll` exists.
6. Launch the game normally. No Steam launch option is required.

To temporarily bypass UE4SS after a problem, add `--disable-ue4ss` to the game's Steam launch options. Remove the argument to enable UE4SS again.

## Building from source

The upstream RE-UE4SS requirements still apply:

- Windows
- Visual Studio/MSVC with C++23 support
- Rust 1.73 or newer
- CMake 3.22 or newer
- Ninja or the Visual Studio build system

Clone this fork and initialize its pinned submodules without using `--remote`:

```powershell
git clone --recurse-submodules https://github.com/Loomirr/LOTDK-UE4SS.git
Set-Location LOTDK-UE4SS
git submodule update --init --recursive
```

Configure and build the shipping target:

```powershell
cmake -B build_cmake_Game__Shipping__Win64 -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
cmake --build build_cmake_Game__Shipping__Win64
```

Consult the upstream [`README.md`](README.md) for alternative generators, profiles, and full build documentation. The final player archive additionally requires the LOTDK settings, signatures, notices, and packaging allowlist maintained outside the generic upstream build target.

## Repository and release boundaries

This repository and its official framework release must not contain:

- The game executable or other retail game files.
- Extracted game assets, SDKs, mappings, or AES keys.
- Saves, logs, crash dumps, personal paths, or generated research captures.
- LOTDK Expanded content, custom suit PAKs, or mod-specific registry payloads.
- Third-party gameplay mods such as SplitScreenMod.

The official player archive contains only the UE4SS framework, LOTDK compatibility data, documented first-party helper files, and the licenses/notices required for redistribution.

## Credits

- **RE-UE4SS contributors** - the upstream UE4SS framework, build system, APIs, Debugging Tools, and modding platform.
- **Narknon and the RE-UE4SS project** - original work distributed under the MIT License retained in [`LICENSE`](LICENSE).
- **3DMen** - the current LOTDK `GUObjectHashTables` compatibility signature, included in binary releases with permission.
- **ThomasPJL** - early LOTDK UE4SS compatibility research, configuration work, and community validation. This fork does not redistribute ThomasPJL's archive or authored gameplay scripts.
- **The LOTDK modding community** - repeated compatibility testing, bug reports, and technical research.

## License and trademarks

The RE-UE4SS source and this fork's modifications are distributed under the repository's [MIT License](LICENSE). Preserve the license and applicable third-party notices when redistributing source or binaries.

LEGO, Batman, DC, Warner Bros., and related names and marks belong to their respective owners and are used here only to identify game compatibility. No publisher-owned game code, executable, SDK, AES key, or extracted retail asset is included.
