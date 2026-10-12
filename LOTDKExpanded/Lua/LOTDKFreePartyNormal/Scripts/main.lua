-- Normal-game party selection. Requires updated LOTDKExpanded with integrated character modes.
local ENABLED_ON_STARTUP = true
local enabled = ENABLED_ON_STARTUP
local prefix = "[LOTDKFreePartyNormal] "

if type(LOTDK_CharacterModesApiVersion) ~= "function" or LOTDK_CharacterModesApiVersion() ~= 1 or type(LOTDK_SetFreePartyNormal) ~= "function" then
    print(prefix .. "INACTIVE: update LOTDK UE4SS/LOTDKExpanded, remove old LOTDKModeAccess and restart.\n")
    return
end

local function apply()
    local ready = LOTDK_SetFreePartyNormal(enabled)
    print(prefix .. (enabled and "ON" or "OFF") ..
        (ready and ". F8 toggles this session. Reopen character selection.\n" or
        " requested; LOTDKExpanded character modes are not active yet.\n"))
end
apply()

if type(RegisterKeyBind) == "function" and Key then
    if type(IsKeyBindRegistered) == "function" and IsKeyBindRegistered(Key.F8) then
        print(prefix .. "F8 is already in use; shortcut was not registered.\n")
    else
        RegisterKeyBind(Key.F8, function()
            enabled = not enabled
            apply()
        end)
    end
end
