#pragma once
#include <string_view>
namespace CrossTraversal {
enum class WorldMode { Unknown, Normal, Mayhem };
inline bool nativeVillain(std::wstring_view path) {
    constexpr std::wstring_view root=L"/Game/AdditionalContent/VillainMode/Characters/Playables/";
    if(!path.starts_with(root) || !path.ends_with(L"_Playable_C")) return false;
    const auto tail=path.substr(root.size());
    return tail.starts_with(L"Joker/BP_Joker_") || tail.starts_with(L"HarleyQuinn/BP_HarleyQuinn_");
}
inline bool eligible(bool enabled,WorldMode mode,bool liveLocalPlayer,bool authority,std::wstring_view path) {
    return enabled && mode==WorldMode::Normal && liveLocalPlayer && authority && nativeVillain(path);
}
inline bool retain(bool enabled,WorldMode mode,bool sameWorld,bool sameAbilityOwner,bool sameLocalController) {
    return enabled && mode==WorldMode::Normal && sameWorld && sameAbilityOwner && sameLocalController;
}
}
