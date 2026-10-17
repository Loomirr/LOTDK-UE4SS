#include "TraversalPolicy.hpp"
#include <iostream>
#include <cstdlib>
int main(){
 using namespace CrossTraversal;
 int count=0;
 auto check=[&](bool v){if(!v){std::cerr<<"FAIL "<<count+1<<'\n';std::exit(1);}++count;};
 const auto joker=L"/Game/AdditionalContent/VillainMode/Characters/Playables/Joker/BP_Joker_Default_Playable.BP_Joker_Default_Playable_C";
 const auto harley=L"/Game/AdditionalContent/VillainMode/Characters/Playables/HarleyQuinn/BP_HarleyQuinn_Default_Playable.BP_HarleyQuinn_Default_Playable_C";
 for(auto path:{joker,harley}) for(bool enabled:{false,true}) for(auto mode:{WorldMode::Unknown,WorldMode::Normal,WorldMode::Mayhem})
 for(bool local:{false,true}) for(bool authority:{false,true})
 check(eligible(enabled,mode,local,authority,path)==(enabled&&mode==WorldMode::Normal&&local&&authority));
 for(auto path:{L"",L"/Game/Mods/Joker/BP_Joker_Default_Playable.BP_Joker_Default_Playable_C",
 L"/Game/AdditionalContent/VillainMode/Characters/Playables/Joker/BP_Joker_Default_Cutscene.BP_Joker_Default_Cutscene_C",
 L"/Game/AdditionalContent/VillainMode/Characters/Playables/JokerNPC/BP_Joker_Default_Playable.BP_Joker_Default_Playable_C",
 L"/Game/Characters/Playables/Batman/BP_Batman_Playable.BP_Batman_Playable_C"})
 check(!eligible(true,WorldMode::Normal,true,true,path));
 const auto customJoker=L"/Game/Mods/JokerCape/Characters/BP_JokerCape_Playable.BP_JokerCape_Playable_C";
 const auto customHarley=L"/Game/Mods/HarleyCape/Characters/BP_HarleyCape_Playable.BP_HarleyCape_Playable_C";
 for(auto [path,scope]:{std::pair{customJoker,L"Pawns.Playable.Joker"},std::pair{customHarley,L"Pawns.Playable.HarleyQuinn"}})
 for(bool registered:{false,true})for(bool enabled:{false,true})for(auto mode:{WorldMode::Unknown,WorldMode::Normal,WorldMode::Mayhem})
 for(bool local:{false,true})for(bool authority:{false,true}) {
     const bool known=registeredVillain(path,path,scope,registered);
     check(known==registered);
     check(eligible(enabled,mode,local,authority,path,known)==(registered&&enabled&&mode==WorldMode::Normal&&local&&authority));
 }
 check(!registeredVillain(customJoker,customHarley,L"Pawns.Playable.Joker",true));
 check(!registeredVillain(customJoker,customJoker,L"Pawns.Playable.JokerFake",true));
 check(!registeredVillain(customJoker,customJoker,L"Pawns.Playable.Joker.Other",true));
 check(!registeredVillain(customJoker,customJoker,L"Pawns.Playable.Batman",true));
 check(!registeredVillain(joker,joker,L"Pawns.Playable.Joker",true));
 check(!registeredVillain(L"",L"",L"Pawns.Playable.Joker",true));
 for(bool enabled:{false,true})for(auto mode:{WorldMode::Unknown,WorldMode::Normal,WorldMode::Mayhem})
 for(bool world:{false,true})for(bool owner:{false,true})for(bool controller:{false,true})
 check(retain(enabled,mode,world,owner,controller)==(enabled&&mode==WorldMode::Normal&&world&&owner&&controller));
 std::cout<<count<<" normal-only traversal policy assertions passed.\n";
}
