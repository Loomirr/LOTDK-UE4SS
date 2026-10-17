# LOTDK UE4SS 0.2.1 — Character Freedom custom-suit support

- Character Freedom now recognizes enabled, validated modded Joker/Harley suits by their exact registered playable class and character scope, not by filename guesses.
- Normal-mode glide/grapple support remains session-only and restricted to locally controlled, authoritative live pawns. Mayhem keeps native ability upgrades and progression.
- Existing mode toggles, grant ownership, cleanup, native characters and the framework's hook/profile behavior are retained.

## Installation

Close the game and back up the current framework first. Extract into LEGOBatmanLotDK so Binaries merges with the existing folder.

- Full ZIP: complete framework installation. Preserve customized settings, mods.txt, saved selections, registry plugins and other mods when upgrading.
- Update-from-0.2 ZIP: requires a complete 0.2 installation. Replaces only LOTDKExpanded's runtime, its matching capability receipt and release documentation. It does not replace settings, mod lists, saves or authored mods, and is not a standalone install.
- Character Freedom's optional Lua controls are separate from traversal upgrades in Mayhem. Existing controls need no script changes. Rebuild old Joker/Harley cape test suits with Batcomputer 1.1.1 Beta 2 to remove the earlier unconditional grants.

## Verification

The exact packaged runtime was tested in an existing game installation: both rebuilt Joker and Harley cape/glider tests worked. The integrated DLL and native/Lua regression suite passed, including 299 normal-only traversal policy assertions. This ZIP has not had a separate clean-install launch test, and not every mission/co-op combination is covered.

UE4SS.dll, dwmapi.dll, signatures and the rest of the 0.2 framework remain byte-identical. Compatibility remains game build 1353284 / Steam build 25720154. No game assets, private character files, test PAKs, settings, saves or logs are included.
