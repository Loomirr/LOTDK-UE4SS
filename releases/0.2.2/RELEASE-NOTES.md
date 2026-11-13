# LOTDK UE4SS 0.2.2 — November game-update compatibility

- Updated Character Freedom's guarded native profile for game build 1364061 / Steam build 26442335.
- Restored the relocated roster, availability, party, preview and traversal grant/remove sites.
- Corrected traversal layout validation so upper padding bytes read by the SDK do not reject valid grapple/glide arrays. Nonzero native flag bytes, incorrect offsets/sizes and unsafe grant ownership remain rejected.
- Existing custom-suit support, mode toggles, normal-only traversal and Mayhem's native upgrade progression are retained. Story-swap experiments are excluded.

## Installation

Close the game and back up your framework. Extract into LEGOBatmanLotDK so Binaries merges with the existing folder.

- Full ZIP: complete framework. Preserve customized settings, mods.txt, saved selections, registry plugins and other mods when upgrading; the full package includes release-default settings and mod lists.
- Update-from-0.2.1 ZIP: requires a complete 0.2.1 installation. Replaces only LOTDKExpanded's DLL, matching capability receipt and release documentation. It preserves settings, saves and other mods. Older installations should use the full package.
- Character Freedom controls ZIP: optional Lua-only F8/F9/F10 controls. Requires this framework; it is not a replacement native runtime. Existing control scripts need no changes. Keep only the controls you want enabled. Remove any old standalone LOTDKModeAccess helper from Mods.

## Verification

The exact packaged DLL passed native/Lua regressions, including 1,069 traversal policy assertions, guarded call-site patching and rollback. All three existing UE4SS Lua signatures matched uniquely in the new executable and live startup succeeded. Joker and Harley grapple/glide were confirmed working in-game; the captured log records a successful normal-mode grant. This is not a separate clean-install test or exhaustive mission/co-op coverage.

UE4SS.dll, dwmapi.dll, signatures and gameplay Lua scripts remain byte-identical to 0.2.1. ZIP CRCs, per-file hashes, privacy checks and update-overlay equivalence passed. No retail game assets, mappings, SDK dumps, private mods, diagnostic logs or saves are included.
