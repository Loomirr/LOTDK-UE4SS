#include "../PreparedClassCache.hpp"
#include <cassert>
#include <iostream>
int main(){
 PreparedClassCache<int> cache;bool validA=true,validB=true;unsigned checks=0;
 auto valid=[&](int h){++checks;return h==1?validA:h==2?validB:false;};
 assert(!cache.ready(L"suit",L"a",L"b",valid));
 cache.remember(L"suit",L"a",L"b",1,2);
 for(int i=0;i<100000;++i)assert(cache.ready(L"suit",L"a",L"b",valid));
 assert(checks==200000);
 validA=false;assert(!cache.ready(L"suit",L"a",L"b",valid));
 validA=true;validB=false;assert(!cache.ready(L"suit",L"a",L"b",valid));validB=true;
 assert(!cache.ready(L"suit",L"changed",L"b",valid));
 assert(!cache.ready(L"suit",L"a",L"changed",valid));
 assert(!cache.ready(L"other",L"a",L"b",valid));
 cache.remember(L"suit",L"new",L"b",1,2);assert(cache.ready(L"suit",L"new",L"b",valid));
 for(int i=0;i<300;++i){cache.remember(std::to_wstring(i),L"a",L"b",1,2);assert(cache.size()<=128);}
 assert(!cache.ready(L"suit",L"new",L"b",valid));
 assert(cache.ready(L"299",L"a",L"b",valid));
 std::cout<<"PASS prepared-class repeat hits, invalid handles/root state, exact paths/suit identity, replacement and bounded eviction\n";
}
