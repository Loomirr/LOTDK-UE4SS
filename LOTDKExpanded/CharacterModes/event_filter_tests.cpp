#include "../EventFilter.hpp"
#include <cassert>
#include <iostream>
#include <string>
int main() {
    using namespace ExpandedEvents;
    for (auto name : {L"SetupButton", L"PreConstruct", L"Construct"}) assert(classify(name) == Generation);
    for (auto name : {L"RequestIdentity", L"TogglePartyPawn", L"RequestLoadIdentity", L"SpawnIdentityIfNeeded",
        L"GetTargetIdentityActorClass", L"IsTargetLoaderReady", L"TryApplyTargetIdentity", L"TryApplyIdentity", L"ApplyIdentityWhenReady"}) assert(classify(name) == LazyScope);
    assert(classify(L"BP_OnClicked") == Selection);
    assert(classify(L"SetPreviewLocked") == Preview);
    assert(classify(L"BP_OnLockedChanged") == Unlock);
    for (auto name : {L"", L"Tick", L"BP_OnPressed", L"BP_OnSelected", L"SetCurrentSelection", L"construct", L"Construct_0"}) assert(classify(name) == 0);
    Cache cache; unsigned calls = 0;
    for (unsigned i=0;i<1000000;++i) assert(cache.get(1,[&]{++calls;return L"Tick";}) == 0);
    assert(calls == 1);
    assert(cache.get(257,[]{return L"Construct";}) == Generation);
    assert(cache.get(1,[&]{++calls;return L"Tick";}) == 0 && calls == 2);
    assert(cache.get(uint64_t(1) | (uint64_t(1)<<32),[]{return L"Construct_0";}) == 0);
    Cache stress;
    for(unsigned i=0;i<10000;++i) {
        const std::wstring text = i%2 ? L"BP_OnClicked" : L"UnrelatedEvent";
        assert(stress.get(i,[&]{return text;}) == classify(text));
    }
    std::cout << "PASS all event names, case/number distinctions, rejection, bounded collision eviction; 1,000,000 repeated unrelated events resolve once\n";
}
