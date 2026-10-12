# Character Freedom — Lua controls for LOTDKExpanded

Install the updated LOTDK UE4SS framework with **integrated character modes API 1** first.
These downloads contain Lua scripts only (plus enable markers and instructions); they do not include a native helper DLL.
Requires Steam executable profile 1344350 and owned/installed Mayhem DLC for DLC characters and Mayhem.

## Upgrade from the old downloads

1. Close the game and back up saves.
2. Move the entire old `Binaries/Win64/ue4ss/Mods/LOTDKModeAccess` folder outside `Mods`.
   If mods.txt contains `LOTDKModeAccess : 1`, disable/remove that entry. Do not remove LOTDKExpanded.
3. Install the updated LOTDK UE4SS framework, including LOTDKExpanded's DLL and matching capabilities.json.
   Preserve your other mod entries. HookEngineTick and HookEndPlay must remain enabled for the supplied traversal support.
4. Extract the desired Lua ZIP's `Binaries` folder into `LEGOBatmanLotDK`.
   Included enabled.txt files enable the Lua mods; no full mods.txt replacement is needed.
5. Restart the game. Keep only one installed copy of each Lua mod.

The old standalone DLL is incompatible with the integrated feature: both would try to own the same native patches.
LOTDKExpanded detects the old DLL and leaves integrated character modes disabled until it is removed.
Old content-only Batcomputer mods with character-mode INIs remain readable; do not reinstall an old ZIP that bundles the helper.

## Controls

- **F9 — Joker & Harley in Normal Mode:** adds their suits/previews to normal mode and supplies the existing locally controlled normal-mode grapple/glide support. Mayhem keeps native traversal progression.
- **F10 — Heroes in Mayhem:** adds native playable hero families to Mayhem and permits switching away from Batman.
- **F8 — Free Party Normal:** permits replacing Batman in normal free roam. Does not unlock characters or bypass mission requirements.

Install any combination. These Lua switches do not control custom characters' explicit Normal/Mayhem/Both policies.

Toggles apply for the current session. Return to native characters and the title screen before disabling, then reload to refresh cached menus.
They are not an emergency unload or save-repair control. Persistent defaults use `ENABLED_ON_STARTUP` in the selected script.
To disable one Lua option before launch, move that mod folder outside Mods (and disable its mods.txt entry if present).

## Verification and troubleshooting

The integrated build passed local compilation, native policy/call-site tests and Lua control checks.
It reuses the previously gameplay-tested crossover implementation, but the **combined LOTDKExpanded build needs its own in-game acceptance test**.

Test normal and Mayhem load/hub entry, first-hover previews, selection, switching away from Batman, traversal and restart.
Keep save backups. Every mission, story role, vehicle and co-op combination is not covered.

If a script reports INACTIVE, update the framework and check for the legacy helper folder.
Check `[LOTDKExpanded:CharacterModes]` in UE4SS.log; unknown executable profiles or failed initialization leave the mode feature inactive.
The Lua keys cannot repair a disabled native feature during that session.
Do not delete saves to troubleshoot startup or roster issues.
