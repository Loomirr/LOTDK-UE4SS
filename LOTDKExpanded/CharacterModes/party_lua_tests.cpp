#include <lua.hpp>
#include <iostream>
int main(int argc,char** argv){
    if(argc!=2)return 1;
    const char* setup[]={
        "calls={}; Key={F8=119}; function LOTDK_SetFreePartyNormal(v) table.insert(calls,v); return true end; function RegisterKeyBind(k,f) assert(k==119); toggle=f end",
        "calls={}; Key={F8=119}; function LOTDK_SetFreePartyNormal(v) table.insert(calls,v); return true end; function RegisterKeyBind() error('collision') end; function IsKeyBindRegistered(k) return true end",
        "function RegisterKeyBind() error('missing-helper binding') end",
        "calls={}; function LOTDK_SetFreePartyNormal(v) table.insert(calls,v); return false end"
    };
    const char* check[]={
        "assert(#calls==1 and calls[1]==true); toggle(); assert(calls[2]==false); toggle(); assert(calls[3]==true)",
        "assert(#calls==1 and calls[1]==true and toggle==nil)",
        "assert(toggle==nil)",
        "assert(#calls==1 and calls[1]==true)"
    };
    for(int i=0;i<4;++i){auto* state=luaL_newstate();luaL_openlibs(state);
        luaL_dostring(state,"function LOTDK_CharacterModesApiVersion() return 1 end");
        if(luaL_dostring(state,setup[i])||luaL_dofile(state,argv[1])||luaL_dostring(state,check[i])){
            std::cerr<<lua_tostring(state,-1)<<'\n';lua_close(state);return 2;}
        lua_close(state);
    }
    std::cout<<"4 Lua startup/toggle/dependency/key-collision scenarios passed.\n";
}
