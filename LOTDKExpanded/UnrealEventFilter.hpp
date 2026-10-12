#pragma once
#include "EventFilter.hpp"
#include <Unreal/UObject.hpp>
namespace ExpandedEvents {
inline unsigned flags(RC::Unreal::UObject* function) {
    if (!function) return 0;
    const auto name = function->GetFName();
    // Display identity preserves the original case-sensitive GetName comparison;
    // the number distinguishes names such as Construct and Construct_0.
    const uint64_t key = uint64_t(name.GetDisplayIndex().ToUnstableInt()) |
        (uint64_t(uint32_t(name.GetNumber())) << 32);
    thread_local Cache cache;
    return cache.get(key, [&] { return function->GetName(); });
}
}
