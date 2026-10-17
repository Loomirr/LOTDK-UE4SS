// Included inside the helper's anonymous namespace. Development candidate only.
namespace Traversal {
using CrossTraversal::WorldMode;
constexpr uintptr_t GrantRva=0x6399a90, RemoveRva=0x63a5ab0;
struct Handles {
    TArray<int32_t> abilities;
    TArray<int64_t> effects;
    TArray<UObject*> attributes;
    TArray<FName> data;
};
static_assert(sizeof(Handles)==0x40);
static_assert(offsetof(Handles,data)==0x30);
struct Owned {
    FWeakObjectPtr pawn,asc,controller,player,world;
    std::array<Handles,2> grants;
    std::array<bool,2> applied{};
};
std::vector<std::unique_ptr<Owned>> owned;
Hook::GlobalCallbackId tickHook{Hook::ERROR_ID},endHook{Hook::ERROR_ID};
bool profileReady=false;
double scanDelay{};
const wchar_t* diagnosticStage=L"idle";

std::optional<bool> boolQuery(UObject* object,const wchar_t* name) {
    auto* fn=object?object->GetFunctionByNameInChain(name):nullptr;
    auto* prop=fn?CastField<FBoolProperty>(fn->GetReturnProperty()):nullptr;
    if(!prop || fn->GetNumParms()!=1 || fn->GetStructureSize()<=0 || fn->GetStructureSize()>64) return {};
    std::vector<uint8_t> args(fn->GetStructureSize(),0);fn->InitializeStruct(args.data());
    object->ProcessEvent(fn,args.data());
    const bool value=prop->GetPropertyValue(prop->ContainerPtrToValuePtr<void>(args.data()));
    fn->DestroyStruct(args.data());return value;
}
WorldMode worldMode(UObject* pawn) {
    // Read the mode using THIS pawn as world context, never the last menu/roster.
    FName tag{STR("GameProgress.Definitions.DLC.VillainModeSave"),FNAME_Find};
    if(tag.ToString()!=STR("GameProgress.Definitions.DLC.VillainModeSave"))return WorldMode::Unknown;
    auto* entry=reinterpret_cast<void*(__fastcall*)(UObject*,FName*)>(image+ProgressEntryRva)(pawn,&tag);
    if(!entry)return WorldMode::Unknown;
    return reinterpret_cast<bool(__fastcall*)(void*)>(image+CompleteRva)(entry)?WorldMode::Mayhem:WorldMode::Normal;
}
bool eligible(UObject* pawn) {
    if(!ready || !profileReady || !villainsInNormal || !pawn ||
       pawn->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject|RF_ArchetypeObject)) || !pawn->GetClassPrivate())return false;
    const auto path=pawn->GetClassPrivate()->GetPathName();
    const bool registered=ExpandedCharacterModesIsRegisteredVillain(path);
    if(!CrossTraversal::nativeVillain(path) && !registered)return false;
    const bool local=boolQuery(pawn,STR("IsPlayerControlled")).value_or(false) &&
        boolQuery(pawn,STR("IsLocallyControlled")).value_or(false);
    if(!local || !boolQuery(pawn,STR("HasAuthority")).value_or(false))return false;
    auto* controller=objectProperty(pawn,STR("Controller"));
    auto* player=objectProperty(controller,STR("Player"));
    // Menu pocket actors and AI partners do not own a local player connection.
    if(!player || !player->GetClassPrivate() || !pawn->GetWorld())return false;
    auto* localClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,STR("/Script/Engine.LocalPlayer"));
    if(!localClass || !player->IsA(localClass))return false;
    return CrossTraversal::eligible(true,worldMode(pawn),true,true,path,registered);
}
bool validAsc(UObject* asc,UObject* pawn) {
    auto* cls=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,STR("/Script/TtGameplayAbilities.TtAbilitySystemComponent"));
    if(!asc || !cls || !asc->IsA(cls) || objectProperty(asc,STR("OwnerActor"))!=pawn)return false;
    return objectProperty(asc,STR("AvatarActor"))==pawn;
}
const wchar_t* retentionFailure(const Owned& item) {
    auto* pawn=item.pawn.Get();auto* asc=item.asc.Get();
    if(!pawn||!asc)return L"pawn or ability system ended";
    if(!ready||!villainsInNormal)return L"crossover disabled";
    const auto mode=worldMode(pawn);
    const bool sameWorld=item.world.Get() && item.world.Get()==reinterpret_cast<UObject*>(pawn->GetWorld());
    const bool owner=validAsc(asc,pawn);
    auto* controller=item.controller.Get();auto* player=item.player.Get();
    const bool controlled=controller&&player&&objectProperty(pawn,STR("Controller"))==controller &&
        objectProperty(controller,STR("Pawn"))==pawn && objectProperty(controller,STR("Player"))==player;
    if(CrossTraversal::retain(true,mode,sameWorld,owner,controlled))return nullptr;
    if(mode!=WorldMode::Normal)return mode==WorldMode::Mayhem?L"entered Mayhem":L"mode unresolved";
    if(!sameWorld)return L"world changed";
    if(!owner)return L"ability ownership changed";
    return L"local controller changed";
}
void removePrivateEntry(UObject* set,FArrayProperty* prop,int index) {
    // These are verified heap arrays on our own transient object. Using the
    // concrete heap operation avoids the SDK's unexported frozen-array branch.
    auto* array=prop->ContainerPtrToValuePtr<FScriptArray>(set);
    auto* inner=prop->GetInner();
    FScriptArrayHelper entries(prop,array);
    inner->DestroyValue(entries.GetRawPtr(index));
    array->Remove(index,1,inner->GetElementSize(),inner->GetMinAlignment());
}
bool filterExisting(UObject* set,UObject* asc,const wchar_t*& reason,bool partial=true) {
    // Only called on a private transient copy. Remove overlapping entries so
    // one shared support ability cannot block the remaining traversal grants.
    auto* granted=CastField<FArrayProperty>(set->GetPropertyByNameInChain(STR("GrantedGameplayAbilities")));
    auto* container=CastField<FStructProperty>(asc->GetPropertyByNameInChain(STR("ActivatableAbilities")));
    auto* itemProperty=container?CastField<FArrayProperty>(container->GetStruct()->GetPropertyByNameInChain(STR("Items"))):nullptr;
    auto* grantStruct=granted?CastField<FStructProperty>(granted->GetInner()):nullptr;
    auto* itemStruct=itemProperty?CastField<FStructProperty>(itemProperty->GetInner()):nullptr;
    auto* grantAbility=grantStruct?CastField<FObjectPropertyBase>(grantStruct->GetStruct()->GetPropertyByNameInChain(STR("Ability"))):nullptr;
    auto* itemAbility=itemStruct?CastField<FObjectPropertyBase>(itemStruct->GetStruct()->GetPropertyByNameInChain(STR("Ability"))):nullptr;
    if(!granted||!itemProperty||!grantAbility||!itemAbility){reason=STR("ability schema");return true;}
    FScriptArrayHelper grants(granted,granted->ContainerPtrToValuePtr<void>(set));
    FScriptArrayHelper items(itemProperty,itemProperty->ContainerPtrToValuePtr<void>(container->ContainerPtrToValuePtr<void>(asc)));
    if(grants.Num()<1 || grants.Num()>64 || items.Num()>2048){reason=STR("ability counts");return true;}
    for(int i=grants.Num()-1;i>=0;--i){
        auto* cls=grantAbility->GetObjectPropertyValue(grantAbility->ContainerPtrToValuePtr<void>(grants.GetRawPtr(i)));
        if(!cls){reason=STR("ability class");return true;}
        for(int j=0;j<items.Num();++j){
            auto* ability=itemAbility->GetObjectPropertyValue(itemAbility->ContainerPtrToValuePtr<void>(items.GetRawPtr(j)));
            if(ability && ability->GetClassPrivate()==cls){if(!partial){reason=L"native ability already present";return true;}removePrivateEntry(set,granted,i);break;}
        }
    }
    auto* attrs=CastField<FArrayProperty>(set->GetPropertyByNameInChain(STR("GrantedAttributes")));
    auto* spawned=CastField<FArrayProperty>(asc->GetPropertyByNameInChain(STR("SpawnedAttributes")));
    // GrantedAttributes is an array of FTtAbilitySet_AttributeSet entries, not
    // an array of classes directly. Its AttributeSet class is nested inside the
    // entry struct. Treating the array inner as FObjectProperty made every core
    // set fail closed before any grant was attempted.
    auto* attrEntry=attrs?CastField<FStructProperty>(attrs->GetInner()):nullptr;
    auto* attrClass=attrEntry?CastField<FObjectPropertyBase>(attrEntry->GetStruct()->GetPropertyByNameInChain(STR("AttributeSet"))):nullptr;
    auto* attrObject=spawned?CastField<FObjectPropertyBase>(spawned->GetInner()):nullptr;
    if(!attrEntry||!attrClass||!attrObject){reason=STR("attribute schema");return true;}
    FScriptArrayHelper requested(attrs,attrs->ContainerPtrToValuePtr<void>(set));
    FScriptArrayHelper current(spawned,spawned->ContainerPtrToValuePtr<void>(asc));
    if(requested.Num()>64||current.Num()>256){reason=STR("attribute counts");return true;}
    for(int i=requested.Num()-1;i>=0;--i){
        auto* cls=attrClass->GetObjectPropertyValue(attrClass->ContainerPtrToValuePtr<void>(requested.GetRawPtr(i)));
        if(!cls){reason=STR("attribute class");return true;}
        for(int j=0;j<current.Num();++j){auto* attr=attrObject->GetObjectPropertyValue(current.GetRawPtr(j));
            if(attr && attr->GetClassPrivate()==cls){if(!partial){reason=L"native attribute already present";return true;}removePrivateEntry(set,attrs,i);break;}}
    }
    if(!partial)return false; // Original, read-only 0.7.5 conflict check.
    // Avoid applying stacking effects on top of native effects: removal must
    // only affect instances owned by this helper.
    auto* effects=CastField<FArrayProperty>(set->GetPropertyByNameInChain(STR("GrantedGameplayEffects")));
    auto* effectEntry=effects?CastField<FStructProperty>(effects->GetInner()):nullptr;
    auto* effectClass=effectEntry?CastField<FObjectPropertyBase>(effectEntry->GetStruct()->GetPropertyByNameInChain(STR("GameplayEffect"))):nullptr;
    auto* active=CastField<FStructProperty>(asc->GetPropertyByNameInChain(STR("ActiveGameplayEffects")));
    auto* activeArray=active?CastField<FArrayProperty>(active->GetStruct()->GetPropertyByNameInChain(STR("GameplayEffects_Internal"))):nullptr;
    auto* activeEntry=activeArray?CastField<FStructProperty>(activeArray->GetInner()):nullptr;
    auto* spec=activeEntry?CastField<FStructProperty>(activeEntry->GetStruct()->GetPropertyByNameInChain(STR("Spec"))):nullptr;
    auto* def=spec?CastField<FObjectPropertyBase>(spec->GetStruct()->GetPropertyByNameInChain(STR("Def"))):nullptr;
    if(!effects||!effectClass||!activeArray||!spec||!def){reason=STR("active effect schema");return true;}
    FScriptArrayHelper wanted(effects,effects->ContainerPtrToValuePtr<void>(set));
    FScriptArrayHelper existing(activeArray,activeArray->ContainerPtrToValuePtr<void>(active->ContainerPtrToValuePtr<void>(asc)));
    if(wanted.Num()>128||existing.Num()<0||existing.Num()>2048){reason=STR("effect counts");return true;}
    for(int i=wanted.Num()-1;i>=0;--i){
        auto* cls=effectClass->GetObjectPropertyValue(effectClass->ContainerPtrToValuePtr<void>(wanted.GetRawPtr(i)));
        if(!cls){reason=STR("effect class");return true;}
        for(int j=0;j<existing.Num();++j){
            auto* effect=def->GetObjectPropertyValue(def->ContainerPtrToValuePtr<void>(spec->ContainerPtrToValuePtr<void>(existing.GetRawPtr(j))));
            if(effect && effect->GetClassPrivate()==cls){removePrivateEntry(set,effects,i);break;}
        }
    }
    trace(L"Traversal missing grants: abilities="+std::to_wstring(grants.Num())+L", attributes="+
        std::to_wstring(requested.Num())+L", effects="+std::to_wstring(wanted.Num())+L".");
    return false;
}
bool validateSet(UObject* set,const wchar_t*& reason,bool nativeGrapple=false) {
    if(!set || !set->GetClassPrivate() || set->GetClassPrivate()->GetPathName()!=STR("/Script/TtGameplayAbilities.TtAbilitySet")){reason=STR("asset class");return false;}
    const std::array<std::pair<const wchar_t*,int>,6> layout={{{STR("GrantedGameplayAbilities"),0x30},
        {STR("GrantedGameplayEffects"),0x40},{STR("GrantedAttributes"),0x50},{STR("GrantedGameplayData"),0x60},
        {STR("GrantedGameplayCueNotifyActorData"),0x70},{STR("GrantedGameplayCueNotifyStaticData"),0x80}}};
    for(auto [name,offset]:layout){auto* prop=CastField<FArrayProperty>(set->GetPropertyByNameInChain(name));
        if(!prop || prop->GetOffset_Internal()!=offset || prop->GetSize()!=16 || prop->GetArrayFlags()!=EArrayPropertyFlags::None){
            static unsigned layoutReports{};
            if(layoutReports++<4) log(L"Traversal layout diagnostic: set="+set->GetPathName()+L" field="+name+
                L" expected_offset="+std::to_wstring(offset)+L" actual_offset="+std::to_wstring(prop?prop->GetOffset_Internal():-1)+
                L" size="+std::to_wstring(prop?prop->GetSize():-1)+L" flags="+
                std::to_wstring(prop?static_cast<unsigned>(prop->GetArrayFlags()):~0U)+L" expected_size=16 expected_flags=0; no grant performed.");
            reason=STR("set layout");return false;}
        FScriptArrayHelper values(prop,prop->ContainerPtrToValuePtr<void>(set));
        if(values.Num()<0||values.Num()>128){reason=STR("set counts");return false;}
        // Generic data grants remove by key, so cannot safely own pre-existing character data.
        if((offset==0x60 || (offset>0x60&&!nativeGrapple)) && values.Num()!=0){reason=name;return false;}
    }
    return true;
}
struct PrivateSet {
    UObject* value{};
    explicit PrivateSet(UObject* source,UObject* outer,const wchar_t*& reason) {
        diagnosticStage=L"resolve SpawnObject";
        // 0.7.6 crashed inside UE4SS's StaticConstructObject adapter, with
        // PrivateSet+0x5c as its return frame, before root/copy/filter operations.
        // Route construction through the game's reflected SpawnObject instead.
        auto* library=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,STR("/Script/Engine.Default__GameplayStatics"));
        auto* fn=library?library->GetFunctionByNameInChain(STR("SpawnObject")):nullptr;
        auto* cls=fn?CastField<FObjectPropertyBase>(fn->GetPropertyByNameInChain(STR("ObjectClass"))):nullptr;
        auto* parent=fn?CastField<FObjectPropertyBase>(fn->GetPropertyByNameInChain(STR("Outer"))):nullptr;
        auto* result=fn?CastField<FObjectPropertyBase>(fn->GetReturnProperty()):nullptr;
        reason=L"SpawnObject reflected signature";
        if(!cls||!parent||!result||fn->GetNumParms()!=3||fn->GetStructureSize()<=0||fn->GetStructureSize()>128)return;
        std::vector<uint8_t> args(fn->GetStructureSize(),0);fn->InitializeStruct(args.data());
        struct ParamsGuard{UFunction* fn;void* args;~ParamsGuard(){fn->DestroyStruct(args);}} guard{fn,args.data()};
        // UE 5.6's generated vtable map has only SetObjectPropertyValueUnchecked;
        // the SDK's older SetObjectPropertyValue wrapper throws before dispatch.
        // Copy the already-resolved object references through FProperty. Parsing
        // a live actor/component path can fail even though the object is valid.
        diagnosticStage=L"copy SpawnObject parameters";
        reason=L"SpawnObject object-reference parameter size";
        if(cls->GetSize()!=sizeof(UObject*)||parent->GetSize()!=sizeof(UObject*))return;
        UObject* requestedClass=source->GetClassPrivate();
        cls->CopyCompleteValue(cls->ContainerPtrToValuePtr<void>(args.data()),&requestedClass);
        parent->CopyCompleteValue(parent->ContainerPtrToValuePtr<void>(args.data()),&outer);
        reason=L"SpawnObject parameter readback mismatch";
        if(cls->GetObjectPropertyValue(cls->ContainerPtrToValuePtr<void>(args.data()))!=source->GetClassPrivate() ||
           parent->GetObjectPropertyValue(parent->ContainerPtrToValuePtr<void>(args.data()))!=outer)return;
        diagnosticStage=L"dispatch SpawnObject";
        trace(L"Traversal diagnostic: reflected ability-set construction starting.");
        library->ProcessEvent(fn,args.data());
        value=result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(args.data()));
        reason=L"SpawnObject returned null";
        if(!value)return;
        trace(L"Traversal diagnostic: reflected ability-set construction completed.");
        diagnosticStage=L"root and copy private set";
        value->SetRootSet();
        constexpr std::array<const wchar_t*,6> names={STR("GrantedGameplayAbilities"),STR("GrantedGameplayEffects"),
            STR("GrantedAttributes"),STR("GrantedGameplayData"),STR("GrantedGameplayCueNotifyActorData"),STR("GrantedGameplayCueNotifyStaticData")};
        try {for(auto name:names){auto* prop=source->GetPropertyByNameInChain(name);
            prop->CopyCompleteValue(prop->ContainerPtrToValuePtr<void>(value),prop->ContainerPtrToValuePtr<void>(source));}}
        catch(...){value->ClearRootSet();value=nullptr;throw;}
    }
    ~PrivateSet(){if(value)value->ClearRootSet();}
    PrivateSet(const PrivateSet&)=delete;
    PrivateSet& operator=(const PrivateSet&)=delete;
};
UObject* loadSet(const wchar_t* path) {
    if(auto* loaded=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,path))return loaded;
    auto* library=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,STR("/Script/Engine.Default__KismetSystemLibrary"));
    auto* fn=library?library->GetFunctionByNameInChain(STR("LoadAsset_Blocking")):nullptr;
    auto* input=fn?CastField<FSoftObjectProperty>(fn->GetPropertyByNameInChain(STR("Asset"))):nullptr;
    auto* result=fn?CastField<FObjectPropertyBase>(fn->GetReturnProperty()):nullptr;
    if(!input||!result||fn->GetNumParms()!=2||fn->GetStructureSize()<=0||fn->GetStructureSize()>256)return nullptr;
    std::vector<uint8_t> args(fn->GetStructureSize(),0);fn->InitializeStruct(args.data());
    struct Destroy{UFunction* fn;void* args;~Destroy(){fn->DestroyStruct(args);}} cleanup{fn,args.data()};
    if(!input->ImportText_Direct(path,input->ContainerPtrToValuePtr<void>(args.data()),library,0,nullptr))return nullptr;
    library->ProcessEvent(fn,args.data());
    return result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(args.data()));
}
void release(Owned& item) {
    auto* asc=item.asc.Get();
    if(asc){
        for(size_t i=0;i<item.grants.size();++i)if(item.applied[i]){
            reinterpret_cast<void(__fastcall*)(Handles*,UObject*)>(image+RemoveRva)(&item.grants[i],asc);
            item.applied[i]=false;
        }
    }
}
void update(float elapsed) {
    if(!profileReady)return;
    for(auto it=owned.begin();it!=owned.end();){
        if(auto* reason=retentionFailure(**it)){
            static unsigned reports{};if(reports++<8)log(L"Traversal released: "+std::wstring(reason));
            auto retiring=std::move(*it);it=owned.erase(it);release(*retiring);
        }else ++it;
    }
    scanDelay-=elapsed;
    if(scanDelay>0 || !ready || !villainsInNormal)return;
    scanDelay=1.0;
    std::vector<UObject*> controllers;UObjectGlobals::FindAllOf(STR("PlayerController"),controllers);
    for(auto* controller:controllers){
        auto* pawn=objectProperty(controller,STR("Pawn"));
        if(!eligible(pawn))continue;
        auto* asc=objectProperty(pawn,STR("AbilitySystemComponent"));
        if(!validAsc(asc,pawn)){
            static unsigned warnings{};if(warnings++<2)log(L"Traversal skipped: active pawn ability-system ownership could not be verified.");
            continue;
        }
        bool tracked=false;for(auto& existing:owned)if(existing->asc.Get()==asc){tracked=true;break;}
        if(tracked)continue;
        auto item=std::make_unique<Owned>();item->pawn=pawn;item->asc=asc;
        item->controller=objectProperty(pawn,STR("Controller"));
        item->player=objectProperty(item->controller.Get(),STR("Player"));
        item->world=reinterpret_cast<UObject*>(pawn->GetWorld());
        struct Pending {
            Owned* item;bool published=false;
            ~Pending(){if(!published)release(*item);}
        } pending{item.get()};
        constexpr std::array<const wchar_t*,2> paths={
            STR("/Game/Characters/Abilities/CoreAbilities/Gliding/AS_Gliding.AS_Gliding"),
            STR("/Game/Characters/Abilities/CoreAbilities/Grappling/AS_Grappling.AS_Grappling")};
        for(size_t i:std::array<size_t,2>{1,0}){try{
            auto* set=loadSet(paths[i]);
            // Loading may dispatch events. Recheck actual context immediately before mutation.
            if(item->pawn.Get()!=pawn || item->asc.Get()!=asc || !eligible(pawn)||!validAsc(asc,pawn))break;
            const wchar_t* reason=STR("unknown");
            if(!validateSet(set,reason,i==1)){
                static unsigned warnings{};
                if(warnings++<4)log(L"Traversal grant skipped ("+std::wstring(reason)+L").");
                continue;
            }
            std::unique_ptr<PrivateSet> missing;
            UObject* grantSet=set;
            bool rejected=false;
            if(i==1){
                // Keep grapple on the exact original set/grant path that worked
                // in 0.7.5; it does not depend on experimental set construction.
                rejected=filterExisting(set,asc,reason,false);
            }else{
                diagnosticStage=L"construct private set";
                missing=std::make_unique<PrivateSet>(set,asc,reason);
                grantSet=missing->value;
                diagnosticStage=L"filter existing grants";
                rejected=!grantSet || filterExisting(grantSet,asc,reason);
            }
            if(rejected){
                static unsigned warnings{};
                if(warnings++<4)log(L"Traversal partial grant skipped ("+std::wstring(reason)+L").");
                continue;
            }
            if(item->pawn.Get()!=pawn || item->asc.Get()!=asc || !eligible(pawn)||!validAsc(asc,pawn))break;
            item->applied[i]=true;
            diagnosticStage=L"native grant";
            reinterpret_cast<void(__fastcall*)(UObject*,UObject*,Handles*,UObject*)>(image+GrantRva)(grantSet,asc,&item->grants[i],nullptr);
            if(item->grants[i].abilities.Num()==0 && item->grants[i].effects.Num()==0 && item->grants[i].attributes.Num()==0){
                reinterpret_cast<void(__fastcall*)(Handles*,UObject*)>(image+RemoveRva)(&item->grants[i],asc);
                item->applied[i]=false;
                static unsigned failures{};if(failures++<2)log(L"Traversal set added no owned handles (already present or grant declined).");
            }
        }catch(const std::exception& error){
            if(item->applied[i]){
                reinterpret_cast<void(__fastcall*)(Handles*,UObject*)>(image+RemoveRva)(&item->grants[i],asc);
                item->applied[i]=false;
            }
            static unsigned failures{};if(failures++<4){const std::string message=error.what();
                log(L"Traversal set "+std::to_wstring(i)+L" failed at "+std::wstring(diagnosticStage)+L": "+std::wstring(message.begin(),message.end()));}
        }
        }
        if(item->applied[0]||item->applied[1]){
            log(L"Normal-mode traversal granted to "+pawn->GetName()+L" (session only; glide="+
                std::to_wstring(item->applied[0])+L", grapple="+std::to_wstring(item->applied[1])+L").");
            owned.push_back(std::move(item));
            pending.published=true;
        }
    }
}
void stop() {
    if(tickHook!=Hook::ERROR_ID){Hook::UnregisterCallback(tickHook);tickHook=Hook::ERROR_ID;}
    if(endHook!=Hook::ERROR_ID){Hook::UnregisterCallback(endHook);endHook=Hook::ERROR_ID;}
    auto retiring=std::move(owned);owned.clear();
    if(!retiring.empty() && IsInGameThread())for(auto& item:retiring)release(*item);
    // Unloading the native DLL during active gameplay is unsupported; restart the game.
    profileReady=false;
}
void start() {
    profileReady=bytes(GrantRva,std::to_array<unsigned char>({0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57})) &&
        bytes(RemoveRva,std::to_array<unsigned char>({0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x55,0x41,0x56,0x41,0x57}));
    if(!profileReady){log(L"Traversal disabled: native grant/remove profile mismatch.");return;}
    endHook=Hook::RegisterEndPlayPreCallback([](Hook::TCallbackIterationData<void>&,AActor* actor,EEndPlayReason){
        for(auto it=owned.begin();it!=owned.end();)if((*it)->pawn.Get()==static_cast<UObject*>(actor)){
            auto retiring=std::move(*it);it=owned.erase(it);release(*retiring);
        }else ++it;
    },{false,true,STR("LOTDKModeAccess"),STR("TraversalCleanup")});
    tickHook=Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&,UEngine*,float elapsed,bool){
        static bool running=false;if(running)return;
        struct Guard{bool& b;Guard(bool& v):b(v){b=true;}~Guard(){b=false;}} guard(running);
        diagnosticStage=L"eligibility and loading";
        try{update(elapsed);}catch(const std::exception& error){static unsigned failures{};if(failures++<4){
            const std::string message=error.what();
            log(L"Traversal exception at "+std::wstring(diagnosticStage)+L": "+std::wstring(message.begin(),message.end()));
        }}
    },{false,true,STR("LOTDKModeAccess"),STR("NormalVillainTraversal")});
    if(endHook==Hook::ERROR_ID||tickHook==Hook::ERROR_ID){stop();log(L"Traversal disabled: lifecycle hooks unavailable.");}
}
}
