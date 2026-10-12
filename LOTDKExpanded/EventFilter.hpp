#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace ExpandedEvents {
enum : unsigned { Selection = 1, Generation = 2, LazyScope = 4, Preview = 8, Unlock = 16 };
constexpr unsigned classify(std::wstring_view name) {
    if (name == L"BP_OnClicked") return Selection;
    if (name == L"SetupButton" || name == L"PreConstruct" || name == L"Construct") return Generation;
    if (name == L"SetPreviewLocked") return Preview;
    if (name == L"BP_OnLockedChanged") return Unlock;
    if (name == L"RequestIdentity" || name == L"TogglePartyPawn" || name == L"RequestLoadIdentity" ||
        name == L"SpawnIdentityIfNeeded" || name == L"GetTargetIdentityActorClass" || name == L"IsTargetLoaderReady" ||
        name == L"TryApplyTargetIdentity" || name == L"TryApplyIdentity" || name == L"ApplyIdentityWhenReady") return LazyScope;
    return 0;
}
// Bounded per-thread memoization. Keys are immutable name identities, never UObject
// addresses. Collisions replace entries; they cannot produce false matches.
class Cache {
    struct Entry { uint64_t key{}; unsigned flags{}; bool valid{}; };
    std::array<Entry, 256> entries{};
public:
    template<class Resolve> unsigned get(uint64_t key, Resolve&& resolve) {
        auto& entry = entries[(key ^ (key >> 32)) % entries.size()];
        if (entry.valid && entry.key == key) return entry.flags;
        const auto flags = classify(resolve());
        entry = {key, flags, true};
        return flags;
    }
};
}
