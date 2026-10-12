# LOTDK UE4SS 0.1.9 — suit-selection performance

- Avoids repeating an expensive whole-game object search when selecting suits. A serial-checked weak menu reference is reused only while the selected button still belongs to that menu; fallback discovery rejects unrelated classes before formatting object names.
- Reuses prepared playable/cutscene classes only when exact suit paths match and both weak references remain valid and rooted. The metadata cache is bounded to 128 identities and does not retain additional game assets.
- Caches ProcessEvent name classifications in a bounded per-thread cache using complete FName identity.
- Avoids evaluating verbose-log arguments when verbose logging is disabled (the runtime default). Existing user settings are not overwritten by the update-only ZIP.
- Precomputes registry-plugin sort metadata and uses a set for duplicate detection. Core-first ordering, all 143 proxy exports and the recovery bypass are retained.
- Adds bounded selection timing and more informative traversal-layout rejection diagnostics. Traversal safety checks remain in place; no ability-grant policy was weakened.

## Verification and limits

The exact included DLLs were tested in an existing game installation. The tester confirmed the suit-selection hitch was resolved and Joker/Harley traversal abilities worked. Native policy, party, traversal, call-site, event-filter, logging, prepared-cache and Lua tests passed. Proxy ordering/duplicate tests and export checks passed. ZIP contents, hashes and privacy checks passed.

This is not a general FPS claim. First-time asset loading can still pause. The complete ZIP has not had a separate clean-install launch test. Game compatibility remains game build 1353284 / Steam build 25720154; incompatible executable profiles still fail closed. The framework, compatibility signatures and optional Lua controls are unchanged from 0.1.8. Old incompatible character assets still require separate rebuilds.

## Installation

Close the game and back up the current framework before updating. Extract into the game's `LEGOBatmanLotDK` directory, so the archive's `Binaries` folder merges with the existing one.

- **Full ZIP:** for a clean installation. It includes default settings and `mods.txt`; preserve your customized settings, extra mod enable entries (Keybinds last), registry plugins and saved selections when upgrading.
- **Update-from-0.1.8 ZIP:** for an existing complete 0.1.8 installation only. Replaces `dwmapi.dll`, LOTDKExpanded's `main.dll`, matching `capabilities.json`, and release documentation. It does not replace settings, mod lists, registry plugins, saves or authored mods. It is not a standalone installation or a Batcomputer updater package.

Move the legacy standalone `LOTDKModeAccess` folder outside Mods if still installed. Optional Character Freedom Lua downloads do not need replacing for this update. No private character/vehicle packs, logs, saves, mappings or game assets are included.
