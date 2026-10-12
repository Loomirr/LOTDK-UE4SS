#pragma once
#include "Policy.hpp"
namespace NormalParty {
inline bool allow(bool known,bool mayhem,bool normalEnabled,bool mayhemEnabled,
                  std::wstring_view pawn,std::wstring_view configuredGroup) {
    return known && ModeAccess::allow_batman_toggle(true,mayhem?mayhemEnabled:normalEnabled,pawn,configuredGroup);
}
}
