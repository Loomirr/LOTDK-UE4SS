#pragma once
#include <string_view>
#include "Policy.hpp"
namespace CrossTraversal {
enum class WorldMode { Unknown, Normal, Mayhem };
inline bool nativeVillain(std::wstring_view path) {
    constexpr std::wstring_view root=L"/Game/AdditionalContent/VillainMode/Characters/Playables/";
    if(!path.starts_with(root) || !path.ends_with(L"_Playable_C")) return false;
    const auto tail=path.substr(root.size());
    return tail.starts_with(L"Joker/BP_Joker_") || tail.starts_with(L"HarleyQuinn/BP_HarleyQuinn_");
}
inline bool registeredVillain(std::wstring_view path,std::wstring_view declaredPlayable,
    std::wstring_view characterScope,bool validEnabled) {
    // Exact enabled manifest identity, not a Joker-like filename or pawn name.
    return validEnabled && path.starts_with(L"/Game/Mods/") && path.ends_with(L"_C") &&
        path==declaredPlayable && (ModeAccess::equal(characterScope,L"Pawns.Playable.Joker") ||
        ModeAccess::equal(characterScope,L"Pawns.Playable.HarleyQuinn"));
}
inline bool eligible(bool enabled,WorldMode mode,bool liveLocalPlayer,bool authority,std::wstring_view path,
    bool registered=false) {
    return enabled && mode==WorldMode::Normal && liveLocalPlayer && authority && (nativeVillain(path)||registered);
}
inline bool retain(bool enabled,WorldMode mode,bool sameWorld,bool sameAbilityOwner,bool sameLocalController) {
    return enabled && mode==WorldMode::Normal && sameWorld && sameAbilityOwner && sameLocalController;
}
}
