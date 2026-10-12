param([string]$NativeBuild = (Join-Path $PSScriptRoot '../../build-ninja'))
$ErrorActionPreference = 'Stop'
$bcBuild = (Resolve-Path $NativeBuild).Path
$bcWorkspace = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bcOutput = Join-Path $bcWorkspace '../output/IntegratedCharacterModes'
New-Item -ItemType Directory -Path $bcOutput -Force | Out-Null
# Reuse the exact ABI flags and libraries of this machine's existing UE4SS build.
# No CMake reconfiguration, external source edits, live-game copying or install target.
$bcNinja = Get-Content -Raw -LiteralPath (Join-Path $bcBuild 'build.ninja')
$bcCompile = [regex]::Match($bcNinja, '(?ms)^build NewSuitSlotNative\\CMakeFiles\\LOTDKExpanded.dir\\dllmain.cpp.obj:.*?(?=^\s*$)').Value
$bcLink = [regex]::Match($bcNinja, '(?ms)^build NewSuitSlotNative\\LOTDKExpanded.dll .*?(?=^\s*$)').Value
function Read-BuildValue([string]$block,[string]$name) {
    $bcMatch = [regex]::Match($block, '(?m)^  ' + $name + ' = (.+)$')
    if (!$bcMatch.Success) { throw "Missing native ABI setting: $name" }
    return $bcMatch.Groups[1].Value.Trim()
}
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $bcVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $bcVsWhere)) { throw 'Run from an x64 Visual Studio developer PowerShell, or install vswhere.' }
    $bcVsRoot = & $bcVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$bcVsRoot) { throw 'Visual Studio C++ tools not found.' }
    $bcVs = Join-Path $bcVsRoot 'Common7/Tools/VsDevCmd.bat'
    $bcEnvironment = & cmd.exe /d /c ('call "' + $bcVs + '" -no_logo -arch=x64 -host_arch=x64 && set')
    $bcCompilerPath = $null
    foreach ($bcLine in $bcEnvironment) { if ($bcLine -match '^([^=]+)=(.*)$') {
        if ($matches[1] -ieq 'Path' -and $matches[2].Contains('MSVC')) { $bcCompilerPath = $matches[2] }
        [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')
    } }
    if ($bcCompilerPath) { $env:Path = $bcCompilerPath }
    if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "Visual Studio environment setup did not expose cl.exe ($($bcEnvironment.Count) output lines)." }
}
$bcDefines = (Read-BuildValue $bcCompile 'DEFINES').Replace('\"','"')
$bcIncludes = Read-BuildValue $bcCompile 'INCLUDES'
$bcFlags = (Read-BuildValue $bcCompile 'FLAGS') -replace '/Zi',''
$bcLibs = (Read-BuildValue $bcLink 'LINK_LIBRARIES') -split '\s+' | ForEach-Object { if ($_ -match '\\') { '"' + (Join-Path $bcBuild $_) + '"' } else { $_ } }
$bcRsp = Join-Path $bcOutput 'build.rsp'
$bcArguments = "$bcDefines`n$bcIncludes`n$bcFlags`n/c /nologo`n/Fo`"$bcOutput/main.obj`"`n`"$PSScriptRoot/main.cpp`""
[IO.File]::WriteAllText($bcRsp,$bcArguments)
& cl.exe "@$bcRsp"
if ($LASTEXITCODE -ne 0) { throw "Mode Access native build failed ($LASTEXITCODE)" }
$bcCoreArgs = "$bcDefines`n$bcIncludes`n$bcFlags`n/c /nologo`n/Fo`"$bcOutput/expanded.obj`"`n`"$bcWorkspace/dllmain.cpp`""
[IO.File]::WriteAllText((Join-Path $bcOutput 'expanded.rsp'),$bcCoreArgs)
& cl.exe "@$bcOutput/expanded.rsp"
if ($LASTEXITCODE -ne 0) { throw 'Expanded core compile failed' }
$bcLinkRsp = Join-Path $bcOutput 'link.rsp'
[IO.File]::WriteAllText($bcLinkRsp,"/DLL /NOLOGO /MACHINE:X64 /INCREMENTAL:NO /OPT:REF /OPT:ICF`n/OUT:`"$bcOutput/main.dll`"`n/IMPLIB:`"$bcOutput/main.lib`"`n`"$bcOutput/main.obj`" `"$bcOutput/expanded.obj`"`n" + ($bcLibs -join ' '))
& link.exe "@$bcLinkRsp"
if ($LASTEXITCODE -ne 0) { throw "Mode Access native link failed ($LASTEXITCODE)" }
foreach($test in 'policy_tests','party_policy_tests','traversal_policy_tests','callsite_tests','event_filter_tests','logging_tests','prepared_class_tests') {
    & cl.exe /nologo /std:c++20 /EHsc /MD "/Fo$bcOutput/$test.obj" "/Fe$bcOutput/$test.exe" "$PSScriptRoot/$test.cpp"
    if($LASTEXITCODE -ne 0){throw "$test compile failed"}
    & "$bcOutput/$test.exe"
    if($LASTEXITCODE -ne 0){throw "$test failed"}
}
& cl.exe /nologo /std:c++20 /EHsc /MD /DRC_LUA_RAW_BUILD_STATIC "/I$bcBuild/../RE-UE4SS/deps/first/LuaRaw/include" "/Fo$bcOutput/lua_tests.obj" "/Fe$bcOutput/lua_tests.exe" "$PSScriptRoot/lua_tests.cpp" "$bcBuild/Game__Shipping__Win64/lib/LuaRaw.lib"
if($LASTEXITCODE -ne 0){throw 'Lua tests compile failed'}
& "$bcOutput/lua_tests.exe" "$PSScriptRoot/../Lua/LOTDKJokerHarleyNormal/Scripts/main.lua" "$PSScriptRoot/../Lua/LOTDKHeroesMayhem/Scripts/main.lua"
if($LASTEXITCODE -ne 0){throw 'Lua tests failed'}
& cl.exe /nologo /std:c++20 /EHsc /MD /DRC_LUA_RAW_BUILD_STATIC "/I$bcBuild/../RE-UE4SS/deps/first/LuaRaw/include" "/Fo$bcOutput/party_lua_tests.obj" "/Fe$bcOutput/party_lua_tests.exe" "$PSScriptRoot/party_lua_tests.cpp" "$bcBuild/Game__Shipping__Win64/lib/LuaRaw.lib"
if($LASTEXITCODE -ne 0){throw 'Party Lua tests compile failed'}
& "$bcOutput/party_lua_tests.exe" "$PSScriptRoot/../Lua/LOTDKFreePartyNormal/Scripts/main.lua"
if($LASTEXITCODE -ne 0){throw 'Party Lua tests failed'}
$exports=& dumpbin.exe /exports "$bcOutput/main.dll"
foreach($name in 'start_mod','uninstall_mod','LOTDKExpanded_CharacterModesApiVersion'){
    if(!($exports -match "\b$name\b")){throw "Missing combined DLL export: $name"}
}
$capability=@{CharacterModesApi=1;DllSha256=(Get-FileHash -LiteralPath "$bcOutput/main.dll").Hash}
[IO.File]::WriteAllText("$bcOutput/capabilities.json",($capability|ConvertTo-Json))
Write-Output 'PASS integrated LOTDKExpanded built and verified; not installed.'
