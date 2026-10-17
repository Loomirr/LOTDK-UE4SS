param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = Get-Content (Join-Path $repo 'deps/first/Unreal/include/Unreal/Hooks/Internal/DetourInstance.hpp') -Raw
# Compile the production decision paths with fake detour operations: no game
# process, native memory patching, or copied implementation of those decisions.
$ensure = [regex]::Match($source, '(?s)static bool EnsureHooked\(\).*?(?=\s*// Returns true if invalid)').Value
$failure = [regex]::Match($source, '(?s)if\(!EnsureHooked\(\)\).*?(?=\s*auto Id = IdCounter)').Value
if (!$ensure -or !$failure) { throw 'Production hook methods were not found; update the test extractor.' }
$fixture = @'
#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#define STR(x) x
enum class LogLevel { Info, Error };
struct Output {
    static inline std::atomic_int info{}, errors{};
    template<LogLevel Level=LogLevel::Info, class... T> static void send(T...) {
        if constexpr (Level == LogLevel::Error) ++errors; else ++info;
    }
};
struct Target { bool ready{}; bool is_ready() const { return ready; } };
struct Options { const char* OwnerModName="test"; const char* HookName="test"; };
struct Fixture {
    bool enabled=false, install=true, activate=true;
    const bool* bShouldHook=&enabled;
    Target target; Target* TargetFunction=&target;
    const char* DetourName="test";
    std::mutex InstallMutex;
    int installs=0, activations=0, registered=0;
    static constexpr int DetourTarget=0, ERROR_ID=-1;
    static inline Fixture* current;
    template<int> static Fixture* GetDetourInstance() { return current; }
    Fixture() { current=this; Output::info=0; Output::errors=0; }
    bool InstallHook() { ++installs; return install; }
    bool ActivateHook() { ++activations; return activate; }
    ENSURE_BODY
    int AddCallback(Options CallbackOptions={}) {
        FAILURE_BODY
        return ++registered;
    }
};
void check(bool ok, const char* name) {
    if (!ok) throw std::runtime_error(name);
    std::cout << "PASS: " << name << '\n';
}
int main() {
    Fixture f;
    std::vector<std::thread> workers;
    for (int i=0; i<16; ++i) workers.emplace_back([] { Fixture::EnsureHooked(); });
    for (auto& worker: workers) worker.join();
    check(Output::info==1 && f.installs==0, "concurrent disabled requests log once and never install");
    check(f.AddCallback()==-1 && f.installs==0 && f.registered==0, "disabled skips discovery, installation and registration");
    check(f.AddCallback()==-1 && Output::info==1 && Output::errors==0, "disabled logs once without a false error");
    f.enabled=true; f.target.ready=true;
    check(f.AddCallback()==1 && f.installs==1 && f.activations==1, "enabled registration still works after a disabled attempt");
    f.activate=false;
    check(f.AddCallback()==-1 && Output::errors==1 && f.registered==1, "activation failure remains an error without registering callback");
    f.activate=true;
    check(f.AddCallback()==2, "activation failure does not poison retry");
    f.install=false; f.target.ready=false;
    int before=f.activations;
    check(f.AddCallback()==-1 && f.activations==before && Output::errors==1, "unavailable target does not activate or duplicate installation diagnostic");
    f.target.ready=true;
    check(f.AddCallback()==-1 && Output::errors==2, "enabled ready-target installation failure remains an error");
    f.install=true;
    check(f.AddCallback()==3, "installation failure does not poison retry");
}
'@
$fixture = $fixture.Replace('ENSURE_BODY', $ensure).Replace('FAILURE_BODY', $failure)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.File]::WriteAllText((Join-Path $output 'HookRegistrationTests.cpp'), $fixture)
Push-Location $output
try {
    & cl.exe /nologo /EHsc /std:c++20 HookRegistrationTests.cpp /Fe:HookRegistrationTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Hook test compilation failed.' }
    & ./HookRegistrationTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Hook registration tests failed.' }
} finally { Pop-Location }
$metadata = Get-Content (Join-Path $repo 'deps/first/Unreal/include/Unreal/Hooks/Internal/Metadata.hpp') -Raw
if ($metadata -notmatch '(?s)Detour == EDetourTarget::ProcessConsoleExec.*?&UnrealInitializer::StaticStorage::GlobalConfig.bHookProcessConsoleExec') {
    throw 'Console hook must respect its discovery configuration.'
}
Write-Output 'PASS: ProcessConsoleExec metadata respects the configured discovery switch'
