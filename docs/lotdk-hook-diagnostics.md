# LOTDK hook diagnostics

Disabled hooks are not detour failures. The hook manager reports a disabled
target once, skips discovery/installation and returns the existing error ID to
callers. No callback is registered. Enabled hooks still attempt installation;
failed attempts remain retryable. Ready targets that fail installation or
activation still produce errors including the requesting callback's owner/name.
Missing targets retain their discovery/installation warnings.

`ProcessConsoleExec` now uses the same configuration switch for callback
registration and address discovery. Previously discovery respected
`HookProcessConsoleExec=0` while registration was marked always-enabled, causing
repeated unavailable-function warnings and misleading detour errors.

In the maintained LOTDK profile, both `HookProcessInternal` and
`HookProcessLocalScriptFunction` are off. The built-in script function discovery
therefore does not run. An absent address in that profile does not establish a
signature failure. Startup reports the resulting custom Blueprint event creation
limitation without the upstream placeholder warning text.

These changes do **not** enable additional hooks or repair/validate their native
addresses. ProcessLocalScriptFunction's built-in discovery follows a call-count
heuristic from ExecuteUbergraph; ProcessConsoleExec uses a virtual-table entry.
Both require build-specific validation and live testing before enabling them.
The LOTDK reflected console-command fallback remains unchanged.

## Verification

From a Visual Studio x64 developer shell:

```powershell
./tools/buildscripts/Test-HookRegistration.ps1 -OutputDirectory <temporary-test-directory>
```

The harness compiles the production registration decision paths against fake
detour operations. It checks disabled/concurrent registration, successful enabled
registration, unavailable targets, installation/activation failures and recovery.
It does not validate native patching, game addresses or gameplay.

Build the full shipping UE4SS target after the tests. Live validation should check
startup, Lua keybinds, Debugging Tools and reflected console commands, custom suit
selection, Joker/Harley traversal, map transitions and shutdown. Preserve the
existing profile and back up the installed DLL before a controlled game test.

The hook changes are stored in `patches/unreal-hook-diagnostics.patch`. CMake
applies the patch to the pinned `deps/first/Unreal` submodule and accepts it on
later configurations. Conflicting dependency edits stop configuration with a
diagnostic. The upstream submodule revision and remote are unchanged.

For an alternate build system, apply the same patch first from the repository
root: `cmake -P cmake/ApplyLotdkUnrealPatch.cmake`.
