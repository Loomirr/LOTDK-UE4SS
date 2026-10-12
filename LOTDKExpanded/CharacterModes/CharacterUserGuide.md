# Character modes in LOTDKExpanded

Character-mode support is part of the **LOTDKExpanded DLL**, distributed with the updated LOTDK UE4SS framework.
Batcomputer does not bundle or install a separate Mode Access DLL.

## Creator workflow

Choose **Normal**, **Mayhem**, or **Both** in **Characters → Character identity & modes**.
Additional suits inherit the character definition's setting. Mayhem requires the owned and installed DLC.

Batcomputer writes `Config/BatcomputerCharacterModes.ini` in the mod's registry plugin.
LOTDKExpanded reads this configuration on Unreal initialization; it is data, not another runtime mod.
Mayhem/Both preflight checks the installed LOTDKExpanded capability receipt and matching DLL hash.
Install the updated framework before building those characters.

Rebuild the complete mod and restart after changing modes. Test roster inclusion, first-hover previews,
selection, switching away and hub entry. Scripted story roles and every co-op/mission combination are not guaranteed.

## Optional Lua controls

The Joker/Harley, Heroes in Mayhem and Free Party Normal downloads are Lua-only controls for the same
integrated runtime. They are optional and do not override custom-character policies.
See [installation and migration](UserGuide.md). The old standalone `Mods/LOTDKModeAccess` folder must be
moved outside Mods before using the integrated runtime.

## Source and build

This directory is the character-mode component source compiled into LOTDKExpanded; its directory name
is retained for source continuity. It does not export a standalone UE4SS mod entry point.
The parent runtime owns initialization, Lua start/stop callbacks and shutdown.

`Build.ps1` compiles both the LOTDKExpanded core and this component using the parent UE4SS ABI,
runs policy/call-site/Lua checks and emits a combined DLL with `capabilities.json`. It does not install
into the game. The combined binary still requires its own in-game acceptance run.

Unknown executable profiles and a detected legacy helper leave character modes inactive.
The feature does not directly write save unlocks, grant entitlements or change the saved mode flag;
keep backups because that does not guarantee every cross-mode gameplay interaction is safe.
