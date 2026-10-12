#include "NormalPartyPolicy.hpp"
#include <iostream>
int main(){
    unsigned count=0;
    for(bool known:{false,true})for(bool mayhem:{false,true})for(bool normal:{false,true})for(bool heroes:{false,true}){
        const bool expected=known&&(mayhem?heroes:normal);
        for(auto pawn:{L"Pawns.Playable.Batman",L"Pawns.Playable.Batman.Default"}){
            if(NormalParty::allow(known,mayhem,normal,heroes,pawn,L"Pawns.Playable.Batman")!=expected)return 1;
            ++count;
        }
        for(auto pawn:{L"Pawns.Playable.HarleyQuinn",L"Pawns.Playable.BatmanExtra",L"",L"Pawns.Playable.Joker"}){
            if(NormalParty::allow(known,mayhem,normal,heroes,pawn,L"Pawns.Playable.Batman"))return 2;
            ++count;
        }
        if(NormalParty::allow(known,mayhem,normal,heroes,L"Pawns.Playable.Batman",L"Pawns.Playable.Joker"))return 3;
        ++count;
    }
    std::cout<<count<<" party policy checks passed.\n";
}
