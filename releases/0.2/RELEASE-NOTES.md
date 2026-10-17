# LOTDK UE4SS 0.2 — clearer hook diagnostics

- Intentionally disabled hooks report once without misleading detour-failure errors.
- ProcessConsoleExec callback registration now respects the same setting as its address discovery.
- Replaces the unfinished Blueprint-event warning with an explanation of the configured capability limit.
- Keeps errors for enabled hooks that fail installation or activation, and allows later retries.

## Verification

The installed candidate passed an existing-installation gameplay test: the tester reported everything working and the log looking good. The shipping build and 10 focused registration checks passed. All 4,074 exported names match the previous framework. The packaged DLL has equal-length compiler-path redactions; executable sections and exports match the accepted candidate. The full ZIP has not had a separate clean-install launch test.

The existing hook profile, compatibility signatures, dwmapi proxy, LOTDKExpanded runtime and Lua helpers are retained from 0.1.9. Optional disabled hooks remain disabled; this release does not establish support for mods that require them. Game compatibility remains build 1353284 / Steam build 25720154.

## Installation

Close the game and back up the framework. Extract into the game's LEGOBatmanLotDK directory so Binaries merges with the existing folder.

- Full ZIP: complete installation. When upgrading, preserve customized settings, mods.txt, registry plugins and saved selections.
- Update-from-0.1.9 ZIP: requires a complete 0.1.9 installation. Replaces only UE4SS.dll and release documentation. Settings, mod lists and authored mods are preserved. This ZIP is not a standalone installation or a Batcomputer updater package.

No authored character/vehicle packs, game assets, saves, mappings or logs are included.
