#pragma once
#include <string>
#include <unordered_map>
// Session-local metadata only. Does not own, root, unroot or release game assets.
template<class Handle> class PreparedClassCache {
    struct Entry {std::wstring playablePath, cutscenePath; Handle playable, cutscene;};
    std::unordered_map<std::wstring, Entry> entries;
public:
    template<class Valid> bool ready(const std::wstring& key, const std::wstring& playable,
        const std::wstring& cutscene, Valid&& valid) const {
        const auto it=entries.find(key);
        return it!=entries.end() && it->second.playablePath==playable && it->second.cutscenePath==cutscene &&
            valid(it->second.playable) && valid(it->second.cutscene);
    }
    void remember(const std::wstring& key,const std::wstring& playable,const std::wstring& cutscene,Handle a,Handle b){
        if(entries.size()>=128 && !entries.contains(key))entries.clear();
        entries.insert_or_assign(key,Entry{playable,cutscene,a,b});
    }
    size_t size() const{return entries.size();}
};
