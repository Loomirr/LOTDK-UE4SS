#pragma once
#include <Mod/CppUserModBase.hpp>
#include <string_view>
// Owned by LOTDKExpanded. This component is never registered as a separate UE4SS mod.
RC::CppUserModBase* CreateExpandedCharacterModes();
// Read only already-validated schema-3 manifests; never load assets from the traversal tick.
bool ExpandedCharacterModesIsRegisteredVillain(std::wstring_view playableClassPath);
