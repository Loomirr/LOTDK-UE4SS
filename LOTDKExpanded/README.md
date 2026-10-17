# LOTDKExpanded runtime source

This is the first-party runtime source corresponding to LOTDK UE4SS 0.2.1. It includes the native suit-selection core, integrated CharacterModes component, optional Lua controls and offline regression tests. No custom character assets or game SDK dumps are included.

The player package uses the exact in-game-tested DLL, not a fresh untested rebuild. Framework baseline: RE-UE4SS `b872ad111394cfabda302dbe744f6893126dfd87`, with this fork's LOTDK changes. CharacterModes executable guards target Steam build 25720154.

## Build environment

`CharacterModes/Build.ps1 -NativeBuild <absolute-build-directory>` is the existing validation harness. It requires the matching configured Ninja build containing the `NewSuitSlotNative/LOTDKExpanded` target and matching UE4SS headers/libraries. It reads that build's ABI flags rather than guessing them. It is not a standalone fresh-clone build command. Run in an x64 Visual Studio developer PowerShell with MSVC, C++23 support and the matching LuaRaw library available. Outputs go to the sibling `output/IntegratedCharacterModes` directory. The script compiles, links, runs native/Lua tests, verifies exports and writes the matching capability receipt; it never installs the result.

The core's internal compatibility label is retained for binary/source continuity. Package version and SHA-256 hashes identify this update. Do not treat an offline test pass as proof of game compatibility after a game update.

## Selection safeguards

Menu reuse uses serial-checked weak references and rechecks button membership. Prepared-class reuse checks both exact paths and valid rooted class handles. Neither cache weakens authoritative selection confirmation or roots new assets. Event filtering keeps case-sensitive event semantics and full FName identity. Timing values for nested phases overlap and must not be added together.

See `CharacterModes/README.md`, its existing license and third-party notices for the integrated component and user workflow.

## Character Freedom suit support

Normal-mode traversal support recognizes original Joker/Harley playables and enabled, validated schema-3 suits registered under their exact character scope and playable class. It never infers identity from a filename. Local-player, authority, mode and lifecycle checks remain mandatory; Mayhem keeps native upgrade progression. The exact 0.2.1 runtime passed the rebuilt Joker/Harley cape tests in an existing game installation. See `releases/0.2.1/RELEASE-NOTES.md` at the repository root for packaging and verification limits.
