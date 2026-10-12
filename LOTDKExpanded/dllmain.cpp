#include <Mod/CppUserModBase.hpp>
#include "CharacterModes/ExpandedCharacterModes.hpp"
#include "UnrealEventFilter.hpp"
#include "VerboseLog.hpp"
#include "PreparedClassCache.hpp"
#include <memory>
#include <DynamicOutput/DynamicOutput.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

class LOTDKExpanded;

namespace
{
    constexpr StringViewType ModTag = STR("[LOTDKExpanded]");
    constexpr StringViewType ReleaseBuild = STR("1.0.0-sep29-compatibility-lotdk-expanded");

    struct SuitModSettings
    {
        bool VerboseLogging = false;
    };

    SuitModSettings SuitSettings{};
    bool SuitSettingsLoaded = false;
    PreparedClassCache<FWeakObjectPtr> PreparedSuitClasses;

    // rf51: every mod verbose log routes through here so one setting can silence
    // them all. Default (VerboseLogging = false) keeps release logs quiet;
    // setting verbose_logging=true enables detailed diagnostics in UE4SS.log.
    // VerboseLog.hpp guards at the call site, before diagnostic arguments run.

    // Always-on, user-facing status lines. Unlike suit_vlog these are NOT gated
    // by verbose_logging -- they're the handful of plain "what just happened"
    // messages (suits loaded, menu injected, suit applied) that help a normal
    // player see the mod working without the full diagnostic firehose.
    template <typename... Args>
    void suit_log(Args&&... args)
    {
        Output::send<LogLevel::Default>(std::forward<Args>(args)...);
    }

    struct SelectionTiming
    {
        const wchar_t* Phase;
        uint64_t Started = GetTickCount64();
        ~SelectionTiming() noexcept
        {
            const auto elapsed = GetTickCount64() - Started;
            if (elapsed < 5) return;
            static std::atomic_uint reports{};
            if (reports.fetch_add(1, std::memory_order_relaxed) >= 128) return;
            try { suit_log(STR("{} Selection timing: phase={} elapsed_ms={} (nested phases overlap)\n"),
                ModTag, Phase, elapsed); } catch (...) {}
        }
    };

    constexpr bool Rf8DisableGameplayEquipmentMutationForCrashIsolation = true;
    constexpr bool Rf9DisableHudPlayerPortraitForCrashIsolation = true;
    constexpr bool Rf9GameplayRequiresFullOperationPreflight = true;
    constexpr bool Rf10RejectPreviewActorGameplayAdoption = true;
    constexpr bool Rf10GameplayRequiresInitializedMaterialSlots = true;
    constexpr bool Rf11CleanRuntimeOnly = true;
    constexpr bool Rf11DisableLegacySavedCutsceneRestore = true;
    constexpr bool Rf11DisableTransientDynamicMaterialRooting = true;
    // rf12: strict crash isolation. Keep only the proven current gameplay
    // texture-parameter path and native-hover menu injection. Do not let old
    // preview-stable direct/reinforce code mutate preview actors, do not run
    // gameplay material replacement operations, and do not capture/restore
    // gameplay material snapshots. This makes the next test answer whether the
    // delayed crash is from live texture parameters or from old material/snapshot
    // systems.
    constexpr bool Rf12StrictTextureOnlyRuntime = true;
    constexpr bool Rf12DisableLegacyPreviewStableDirectApply = false;
    // rf32: re-enable gameplay set_material for ALL suits so the gameplay apply
    // path matches the preview path (and matches every other suit). The original
    // rf12 "strict texture-only" crash isolation was to avoid the delayed crash
    // that we later traced to GC of applied assets â€” that is now fixed by rooting
    // assets/MIDs via SetRootSet() (retain_suit_asset / CreateDynamicMaterialInstance).
    // Cape set_material was already re-allowed in gameplay (rf19) without issue, so
    // body/face/hair/torso set_material is safe to apply too. This fixes Thomas,
    // whose entire look is set_material (CharacterMesh0 body, Face, NeckPeg hair)
    // and which previously had those three ops skipped only during gameplay.
    constexpr bool Rf12DisableGameplaySetMaterialOpsForCrashIsolation = false;
    constexpr bool Rf12DisableGameplaySnapshotsAndMaterialBaselines = true;
    constexpr bool Rf14DisablePreviewStableDirectReinforce = true;
    // rf15: reintroduce only two proven-safe pieces on top of the rf14
    // stable manual-F6 baseline: pawn-possess asset preloading and short UI
    // text refresh windows. Keep native-hover injection, HUD, equipment,
    // gameplay set_material, snapshots, and preview reinforce disabled.
    constexpr bool Rf15PawnPossessPreloadEnabled = true;
    constexpr bool Rf15UiTextRefreshEnabled = true;
    constexpr bool Rf15ButtonTextRefreshEnabled = true;
    // rf16: manual-F6 injection is the active injection path. Native suit
    // button hover/watch paths were still firing while scrolling and caused
    // lag/freeze in rf15.
    constexpr bool Rf16DisableNativeButtonWatchers = true;
    constexpr bool Rf17CancelPreviewGateOnCustomUnhover = true;
    constexpr bool Rf17MaturePreviewObservationOnly = true;

    // rf79: ignore an unhover that fires within this window of the hover arming.
    // Some levels (e.g. Gotham) emit a spurious BP_OnUnhover tens of ms after the
    // cursor lands on a custom button while it is still hovered, which cancelled
    // the stable-hover before it could trigger -> previews never updated there.
    // A genuine cursor-leave dwells longer than this before firing.
    constexpr uint64_t Rf79UnhoverDebounceMs = 350;
    constexpr bool Rf18CancelPreviewGateOnHoverSwitch = true;
    constexpr uint64_t Rf18PreviewCancelLogThrottleMs = 500ULL;
    constexpr uint64_t Rf18UnhoverSkipLogThrottleMs = 500ULL;
    constexpr bool Rf19StableHoverFallbackPreviewScan = true;
    constexpr bool Rf19AllowGameplayCapeSetMaterialOps = true;
    constexpr bool Rf20ManualF6PrefersCurrentGridSignal = true;
    constexpr bool Rf20EnableDirectHudFacingIconOverride = true;
    constexpr bool Rf21DeferGameplayApplyWhileSuitMenuActive = true;
    constexpr bool Rf21EnableEquipmentSwapsAfterGameplay = true;
    constexpr bool Rf21ClearPreviewGenerationOnGridBind = true;
    constexpr bool Rf22BoundLiveSuitGridGameplayDefer = true;
    constexpr uint64_t Rf22LiveSuitGridExtraPreviewDeferMs = 3000ULL;
    // rf97: never mutate the gameplay pawn while the user is actively previewing
    // a DIFFERENT suit in an open Batman suit menu. The rf22 bounded defer above
    // EXPIRES after the window, after which the committed suit's gameplay re-apply
    // would proceed mid-preview -- visibly interrupting the hovered preview and
    // forcing a rehover (user report). This keeps deferring as long as a preview
    // of a non-committed suit is live (preview_work_busy_for_gameplay), which is
    // false at real commit time (preview index == committed) so first-commit
    // apply is never blocked. The pawn already wears the committed suit from that
    // first commit, so deferring re-apply while the menu is open is harmless.
    constexpr bool Rf97DeferGameplayWhilePreviewingDifferentSuit = true;
    constexpr bool Rf23PreviewRequiresReadySourceMaterials = true;
    constexpr bool Rf24PreviewActivityWakesSoftPostApplyIdle = true;
    constexpr bool Rf25CustomClickWakesSoftPostApplyIdle = true;
    constexpr bool Rf26PreloadProgressNotifications = true;
    constexpr bool Rf26KeepAwakeDuringCustomPreviewActivity = true;
    constexpr uint64_t Rf26PreloadNotificationThrottleMs = 250ULL;
    constexpr bool Rf27ForgetPreviewHoverMemoryOnFreshHover = true;
    constexpr bool Rf27DeferGameplayOnLiveGridPreviewMismatch = true;
    constexpr uint64_t Rf27FinalNotificationHideMs = 10000ULL;
    constexpr bool Rf28DeferGameplayCosmeticsUntilSuitGridClosed = true;
    constexpr uint64_t Rf28GameplayPostMenuCloseSettleMs = 1000ULL;
    constexpr bool Rf29MenuInjectionOnlyAssetPreload = false;
    // rf82: was 60 frames (~1s). That delay made the preload notification show
    // "1/N" and then sit there for a second before the first asset loaded, which
    // looked stuck. ReceivePossessed already fires after the pawn is possessed
    // (level loaded enough), so only a tiny settle is needed -- start loading
    // essentially as soon as the notification appears.
    constexpr int32_t Rf36PawnPossessionPreloadDelayFrames = 6;

    // rf82: how many assets to prime per on_update tick. Was effectively 1 while
    // progress notifications are on, so each asset paid a full frame of other
    // on_update work + frame pacing + a notification refresh between loads.
    // Processing several per tick cuts that inter-load overhead (the per-asset
    // load/scan cost itself is unchanged). Kept modest to avoid long hitches.
    constexpr int32_t Rf82PreloadStepsPerTick = 4;
    constexpr bool Rf38PreviewApplyUsesExplicitConfigIndex = true;
    constexpr bool Rf38RejectNonePreviewActors = true;
    constexpr bool Rf39PreviewExternalTexturePrimeAllowed = true;
    constexpr bool Rf39CapturedRetryRefreshesPreviewActor = true;
    constexpr bool Rf40FullTransientResetOnFreshNativeGrid = true;
    constexpr bool Rf40MenuInjectionSoftIconNoUObjectScan = true;
    constexpr bool Rf40IgnoreMenuProcessEventsDuringGridInjection = true;
    constexpr bool Rf41ProgressNotificationTextRefreshOnly = true;
    constexpr bool Rf42RepairHudPortraitAfterNativeChange = true;
    constexpr uint64_t Rf42HudPortraitRepairDelayMs = 300ULL;
    constexpr int32_t Rf42HudPortraitRepairMaxNoPawnRetries = 6;
    constexpr bool Rf43QuiesceDuringLevelTransitions = true;
    constexpr uint64_t Rf43PostPossessionSettleMs = 2500ULL;
    constexpr uint64_t Rf43TransitionWaitLogThrottleMs = 1500ULL;
    constexpr bool Rf45EventDrivenSavedCutsceneApply = true;
    constexpr uint64_t Rf45CutsceneApplyDelayMs = 1000ULL;
    constexpr uint64_t Rf45CutsceneApplyRetryMs = 500ULL;
    constexpr uint64_t Rf45CutsceneApplyDeadlineMs = 8000ULL;
    constexpr bool Rf30ApplyAfterTargetSourcePawnObserved = true;
    constexpr uint64_t Rf30TargetSourcePawnSettleMs = 1000ULL;
    // rf33: re-enable live equipment mutation (Thomas NinjaStar/FoamGun). It was
    // disabled in rf31 after a crash at EquipmentContainerSwap MUTATION_BEGIN, but
    // that crash happened while gameplay set_material was being skipped (rf12),
    // leaving the pawn in a broken material state before equipment mutation. With
    // rf32 applying materials correctly, the equipment swap runs against a valid
    // pawn. WATCH: if the game still crashes right after MUTATION_BEGIN, set this
    // back to true â€” equipment mutation itself is then the culprit.
    constexpr bool Rf31DisableLiveEquipmentMutationForStableSuitApply = false;
    constexpr bool Rf16ManualF6InjectionOnly = true;
    // rf166/rf170: first "chameleon donor DCMD" test. Native/generated suit
    // configs temporarily copy their generated DCMD payload onto the unlocked
    // TheBatman2025 DCMD while their custom button is hovered/selected. The
    // original donor fields are restored only when the real native
    // TheBatman2025 button is hovered/selected, so other custom suits sharing
    // the bridge can keep the preview/identity alive between hover and rotate.
    constexpr bool Rf166DonorDcmdHoverPatchEnabled = true;
    // Native V2 test lane: only JSON manifests with schema_version >= 2 and a
    // dcmd_asset are loaded. Old V1 runtime-operation suits are intentionally
    // skipped so multi-suit DCMD/button testing is clean and repeatable.
    constexpr bool NativeV2JsonOnlyMode = true;
    constexpr int32_t NativeV2MinimumSchemaVersion = 2;
    // rf169: the donor DCMD bridge is now proven. Keep the hover path lean: one
    // summary line is enough, and the full property-before/after dumps can be
    // re-enabled here if we need to investigate another bad copy.
    constexpr bool Rf169DonorDcmdDetailedPatchLogs = false;
    constexpr bool Rf169DonorDcmdPathFallbackOnlyOnPartialCopy = true;

    // rf71: a previewed suit can target a material slot that is genuinely empty on
    // the preview actor (CreateDynamicMaterialInstance cannot seed it), so the
    // apply mask never reaches 0x3F and the preview PARTIAL_RETRYs forever ("never
    // updates"). After this many retries, accept the partial result and stop, so
    // the slots that DID apply show instead of looping.
    constexpr int32_t Rf71PreviewPartialAcceptRetries = 8;

    // rf71: throttle for the auto-inject-on-menu-open check (ms between probes).
    constexpr uint64_t Rf71AutoInjectCheckIntervalMs = 400;

    // rf77: frames to wait after a reinject before re-selecting the committed
    // suit's button, so the native menu has asserted (and we then override) its
    // own selection. ~15 frames is past the native settle without a visible delay.
    constexpr int32_t Rf77CommittedSelectDelayFrames = 15;
    constexpr const wchar_t* NativeSuitBridgeSourceTag =
        STR("Pawns.Playable.Batman.TheBatman2025");
    constexpr const wchar_t* NativeSuitBridgeProgressTag =
        STR("GameProgress.Definitions.Characters.Batman.TheBatman2025");
    constexpr const wchar_t* NativeSuitBridgeSourceActorClassContains =
        STR("BP_Batman_TheBatman2025_Playable_C");
    constexpr const wchar_t* NativeSuitBridgePlayableClassPath =
        STR("/Game/Characters/Minifig/Batman/BP_Batman_TheBatman2025_Playable.BP_Batman_TheBatman2025_Playable_C");
    constexpr const wchar_t* NativeSuitBridgeCutsceneClassPath =
        STR("/Game/Characters/Minifig/Batman/BP_Batman_TheBatman2025_Default_Cutscene.BP_Batman_TheBatman2025_Default_Cutscene_C");
    // rf206: the icon each donor's NATIVE button is supposed to show.
    //
    // Bridging copies our DCMD -- including its UIMetaData -> our DA_UIMD, whose SuitIcon is
    // the custom suit's -- onto a donor, and the native button renders its icon from that same
    // DCMD. So while patched, the native button shows our icon. We keep the copy (the suit
    // needs its own icon everywhere UIMetaData is read) and instead re-assert the donor's own
    // icon straight onto the native button, the same way rf136 re-asserts its unlocked state.
    constexpr const wchar_t* NativeSuitBridgeSourceButtonIcon =
        STR("/Game/UI/Icons/Suits/T_UI_IconSuit_Batman_TheBatman2025_BCA.T_UI_IconSuit_Batman_TheBatman2025_BCA");
    constexpr const wchar_t* NativeSuitBridgeAltSourceButtonIcon =
        STR("/Game/UI/Icons/Suits/T_UI_IconSuit_Batman_Begins_BCA.T_UI_IconSuit_Batman_Begins_BCA");

    constexpr const wchar_t* NativeSuitBridgeAltSourceTag =
        STR("Pawns.Playable.Batman.Begins");
    constexpr const wchar_t* NativeSuitBridgeAltProgressTag =
        STR("GameProgress.Definitions.Characters.Batman.Begins");
    constexpr const wchar_t* NativeSuitBridgeAltSourceActorClassContains =
        STR("BP_Batman_Begins_Playable_C");
    constexpr const wchar_t* NativeSuitBridgeAltPlayableClassPath =
        STR("/Game/Characters/Minifig/Batman/BP_Batman_Begins_Playable.BP_Batman_Begins_Playable_C");
    constexpr bool NativeSuitBridgeAutoHandoffEnabled = false;
    // rf171: command-only measurement for the single-donor self-bounce plan.
    // Ctrl+F8 requests this real/native Batman tag, then samples whether the
    // TheBatman2025 donor identity becomes unspawned (destroyed) or remains
    // spawned/parked. This does NOT patch or destroy anything.
    //
    // rf172: Begins was a tempting reset tag, but the log proved that even after
    // a session-only RuntimeOverride its party-member RequestLoadIdentity still
    // returned false, so it never spawned. Use base Batman as the reset probe: it
    // should be the safest/most universal real Batman identity.
    constexpr const wchar_t* NativeSuitSelfBounceProbeTag =
        STR("Pawns.Playable.Batman.Batman");
    constexpr const wchar_t* NativeThomasWayneDefaultSourceTag =
        STR("Pawns.Playable.ThomasWayne.Default");
    constexpr const wchar_t* NativeThomasWayneDefaultProgressTag =
        STR("GameProgress.Definitions.Characters.ThomasWayne.Casual");
    constexpr const wchar_t* NativeThomasWayneDefaultPlayableClassPath =
        STR("/Game/Characters/Minifig/ThomasWayne/BP_ThomasWayne_Casual.BP_ThomasWayne_Casual_C");
    constexpr const wchar_t* NativeThomasWayneDefaultCutsceneClassPath =
        STR("/Game/Characters/Minifig/ThomasWayne/BP_ThomasWayne_Casual_CUT.BP_ThomasWayne_Casual_CUT_C");
    // Command-only native suit probe lane.
    //
    // Important: this intentionally points at the GUI-generated Electric suit
    // while we validate generated DCMD/UIMD/pawn-tag behavior. The release JSON
    // for Electric can remain in the old runtime-operations format during this
    // phase; F8/F9 are the isolated native-character test controls.
    constexpr const wchar_t* NativeGeneratedElectricSlotId =
        STR("batman_electric_suit");
    constexpr const wchar_t* NativeGeneratedElectricNativeTestSlotId =
        STR("batman_electric_native_test");
    constexpr const wchar_t* NativeGeneratedElectricRegisteredTagButtonTestSlotId =
        STR("batman_electric_registered_tag_button_test");
    constexpr const wchar_t* NativeGeneratedElectricTargetPawnTag =
        STR("Pawns.Playable.Batman.Electric");
    constexpr const wchar_t* NativeGeneratedElectricDcmdAssetPath =
        STR("/Game/Mods/ElectricLBM2/Characters/DA_DCMD_Batman_Electric_Playable.DA_DCMD_Batman_Electric_Playable");
    // Isolated registration specimen. The normal generated /Game/Mods DCMD is
    // intentionally bridge-tagged as TheBatman2025; this separately mounted
    // asset is cooked with Pawns.Playable.Batman.Electric for the native
    // registry experiment.
    constexpr const wchar_t* NativeGeneratedElectricDevScanDcmdAssetPath =
        STR("/Game/Mods/ElectricLBM2/Characters/DA_DCMD_Batman_Electric_Playable.DA_DCMD_Batman_Electric_Playable");
    constexpr const wchar_t* NativeGeneratedElectricStartupRegistryProofDcmdAssetPath =
        STR("/Game/Mods/ElectricLBM2/Characters/DA_DCMD_Batman_Electric_Playable.DA_DCMD_Batman_Electric_Playable");
    constexpr const wchar_t* NativeGeneratedElectricLegacyDcmdAssetPath =
        STR("/Game/Characters/Minifig/Batman/DA_DCMD_Batman_Electric_Playable.DA_DCMD_Batman_Electric_Playable");
    constexpr const wchar_t* NativeGeneratedElectricPlayableClassPath =
        STR("/Game/Mods/ElectricLBM2/Characters/BP_Batman_Electric_Playable.BP_Batman_Electric_Playable_C");
    constexpr const wchar_t* NativeGeneratedElectricCutsceneClassPath =
        STR("/Game/Mods/ElectricLBM2/Characters/BP_Batman_Electric_Cutscene.BP_Batman_Electric_Cutscene_C");
    constexpr const wchar_t* NativeGeneratedElectricUIMDAssetPath =
        STR("/Game/Mods/ElectricLBM2/UI/DA_UIMD_Batman_Electric.DA_UIMD_Batman_Electric");
    constexpr const wchar_t* NativeGeneratedElectricIconAssetPath =
        STR("/Game/Mods/ElectricLBM2/ElectricSuitIcon.ElectricSuitIcon");
    constexpr const wchar_t* DefaultSuitIconPath =
        STR("/Game/UI/Icons/Characters/T_UI_IconChar_ThomasWayne_Casual_Menu_BCA.T_UI_IconChar_ThomasWayne_Casual_Menu_BCA");
    constexpr const wchar_t* DefaultSuitBodyMaterialPath =
        STR("/Game/Characters/Minifig/ThomasWayne/Material/MI_ThomasWayne_EoM.MI_ThomasWayne_EoM");
    constexpr const wchar_t* DefaultSuitFaceMaterialPath =
        STR("/Game/Characters/Attachments/Face/FACE_ThomasWayne/MI_FACE_ThomasWayne.MI_FACE_ThomasWayne");
    constexpr const wchar_t* DefaultSuitHairMaterialPath =
        STR("/Game/Characters/Attachments/Hair/SlickBack/MI_HAIR_SlickBack_Black_EOM.MI_HAIR_SlickBack_Black_EOM");
    constexpr const wchar_t* DefaultSuitHairMeshPath =
        STR("/Game/Characters/Attachments/Hair/SlickBack/SM_HAIR_SlickBack.SM_HAIR_SlickBack");

    // ---- Native DCMD virtual-registry redirect (hook-based) ----
    // Rather than mutating UTtPawnMetaDataSystem's internal tag->DCMD map (native,
    // unreflected, crash-prone), we intercept its reflected getters via the global
    // ProcessEvent post-callback and:
    //   (a) return our loaded DCMD when the game looks up our pawn tag, and
    //   (b) advertise our tag in the enumerator results so character-select lists
    //       our suit as a real native slot (icon/name/pawn all flow from the DCMD).
    // This is the proven old "JsonMetadataRedirect" idea extended from the single
    // lookup getter to the full getter set so the suit actually appears in the menu.
    std::atomic_bool NativeDcmdRedirectActive{false};
    // Command-only probe: F8 arms the old Electric tag->DCMD redirect and also
    // lets the injected Electric custom button report the custom Electric pawn
    // tag instead of the normal donor tag. This is intentionally separate from
    // the release ping-pong donor bridge.
    std::atomic_bool NativeDcmdRedirectElectricButtonTagProbeActive{false};
    std::atomic<UObject*> NativeDcmdRedirectDcmd{nullptr};
    // rf224b: retain the exact objects involved in the command-only native
    // registration proof so Ctrl+F9 can distinguish UObject lifetime loss,
    // root-flag loss, metadata-system replacement, and private-registry
    // replacement without trusting StaticFindObject. These are diagnostic
    // addresses only; every later dereference is gated by a fresh UObject-array
    // liveness check.
    std::atomic<UObject*> RegisteredElectricPawnMetadataProbeObject{nullptr};
    std::atomic<UObject*> RegisteredElectricPawnMetadataProbeSystem{nullptr};
    std::atomic<void*> RegisteredElectricPawnMetadataProbeRegistry{nullptr};
    std::atomic_bool RegisteredElectricPawnMetadataInjectionProbeAttempted{false};
    std::atomic_bool RegisteredElectricPawnMetadataLoadProbeAttempted{false};
    std::atomic_bool RegisteredElectricPawnMetadataSpawnProbeAttempted{false};
    std::atomic_bool RegisteredElectricPawnMetadataApplyProbeAttempted{false};
    std::atomic_bool RegisteredElectricPawnMetadataImmediateRequestQueued{false};
    FName NativeDcmdRedirectTag{};
    FName NativeDcmdRedirectParentTag{};
    std::atomic<uint64_t> NativeDcmdRedirectLookupHits{0};
    std::atomic<uint64_t> NativeDcmdRedirectEnumHits{0};

    // Log-only experiment: observe reflected calls into
    // UTtPawnMetaDataSystem::GetMetaDataForPawnTag before we try overriding
    // custom tags. This must never mutate ReturnValue.
    std::atomic<uint64_t> PawnMetadataLookupProbeHits{0};

    struct DonorDcmdPropertySnapshot
    {
        StringType PropertyName{};
        StringType PropertyType{};
        StringType ExportedValue{};
        bool Captured{false};
    };

    struct DonorDcmdBridgePatchState
    {
        bool OriginalSnapshotCaptured{false};
        bool Active{false};
        int64_t ActiveSuitIndex{-1};
        StringType ActiveSlotId{};
        StringType ActiveGeneratedDcmd{};
        StringType ActivePatchScope{};
        StringType SnapshotMetadataName{};
        std::vector<DonorDcmdPropertySnapshot> OriginalProperties{};
    };

    DonorDcmdBridgePatchState TheBatman2025DonorDcmdBridge{};
    DonorDcmdBridgePatchState BatmanBeginsDonorDcmdBridge{};

    // A lightweight global ProcessEvent observer captures the injected
    // button's hover/selection events and the source preview actor.
    Hook::GlobalCallbackId GlobalSelectionProcessEventTracePreCallbackId{Hook::ERROR_ID};
    Hook::GlobalCallbackId GlobalSelectionProcessEventTracePostCallbackId{Hook::ERROR_ID};
    thread_local bool GlobalSelectionProcessEventTraceInternal{false};
    thread_local bool NativeManifestProcessEventInternal{false};
    thread_local int32_t NativeSuitSelectionMenuGenerationDepth{0};

    struct PendingNativeSuitSelectionEvent
    {
        UObject* Button{};
        UFunction* Function{};
        FWeakObjectPtr SuitMenu{};
        StringType EventName{};
        StringType PawnTag{};
        bool LiveMenuSignal{false};
        bool MenuGenerationSettling{false};
    };

    thread_local std::vector<PendingNativeSuitSelectionEvent>
        PendingNativeSuitSelectionEvents{};

    // The button publishes its selected flag after BP_OnClicked returns. Keep only
    // a weak reference to the owning suit menu, then confirm the click through the
    // menu's authoritative GetSelectedOption result on a later game-thread event.
    struct PendingNativeSuitSelectionConfirmation
    {
        FWeakObjectPtr SuitMenu{};
        StringType SuitMenuName{};
        StringType PawnTag{};
        StringType EventName{};
        uint64_t QueuedAtMs{};
        uint64_t NextPollMs{};
        uint64_t DeadlineMs{};
        uint64_t Generation{};
        int32_t PollCount{};
    };

    std::mutex PendingNativeSuitSelectionConfirmationMutex{};
    PendingNativeSuitSelectionConfirmation
        PendingNativeSuitSelectionConfirmationState{};
    bool PendingNativeSuitSelectionConfirmationValid{false};
    uint64_t PendingNativeSuitSelectionConfirmationGeneration{0};
    std::atomic_bool NativeSuitSelectionConfirmationPending{false};
    thread_local int32_t NativeManifestStaticConstructObjectDepth{0};
    std::atomic_flag NativeManifestProcessEventBusy = ATOMIC_FLAG_INIT;

    // rf254: bounded, command-only, log-only observation of the game's proven
    // native suit-selection commit path. The rf252/rf253 direct PartyMember
    // experiments were accepted but never spawned the target identity; the
    // actual native menu selection clearly performs additional private/native
    // preparation. This trace records only relevant reflected UI, party,
    // identity-loader, and possession calls while the user selects a known-good
    // schema-3 suit from the native menu. It never changes params or UObjects.
    std::atomic_bool NativeSuitCommitTraceArmed{false};
    std::atomic<uint64_t> NativeSuitCommitTraceDeadlineMs{0};
    std::atomic<uint64_t> NativeSuitCommitTraceHits{0};
    constexpr uint64_t NativeSuitCommitTraceWindowMs = 45000ULL;
    constexpr uint64_t NativeSuitCommitTraceMaxHits = 600ULL;

    struct ScopedAtomicBoolFlag
    {
        std::atomic_bool& Flag;
        bool PreviousValue{false};

        explicit ScopedAtomicBoolFlag(std::atomic_bool& flag)
            : Flag(flag), PreviousValue(flag.exchange(true))
        {
        }

        ~ScopedAtomicBoolFlag()
        {
            Flag.store(PreviousValue);
        }
    };

    // rf7: once the custom gameplay suit, equipment, and HUD portrait have
    // completed, stop the old runtime update/process-event watchers. Wake only
    // when the user interacts with a suit menu button again or a matching
    // frontend cutscene/showcase animation signal appears. Kept outside the main runtime
    // state struct so this trim is easy to revert and cannot disturb rf5 state
    // layout/timers before gameplay applies.
    std::atomic_bool RuntimeSoftPostApplyIdle{false};
    std::atomic_int64_t RuntimeSoftPostApplyIdleSuitIndex{-1};
    uint64_t RuntimeSoftPostApplyIdleSinceMs{0};
    UObject* RuntimeSoftPostApplyIdleActor{nullptr};
    StringType RuntimeSoftPostApplyIdleActorName{};


    // The old research build logged and exported hundreds of ProcessEvent calls
    // during every character-menu refresh. Keep the code for manual research,
    // but disable it in the normal runtime path.
    // This build intentionally makes the custom visual slot use the configured
    // source suit's native option data. The game therefore handles unlock,
    // preview loading, equipping, and saving as TheBatman2025 while the DLL
    // applies Thomas cosmetic materials only to the actor shown for our button.
    std::atomic_bool SuitInternalSourceTagEnabled{true};
    std::atomic_bool SuitMaterialSwapEnabled{false};

    std::atomic_bool CustomSlotClickPending{false};
    std::atomic_bool OtherSuitClickPending{false};
    std::atomic_bool OtherSuitClickShouldRestoreBridgeDcmd{false};
    std::atomic_bool CustomSlotHoverPending{false};
    std::atomic_bool PreviewHoverCaptureArmed{false};
    std::atomic_bool CapturedPreviewApplyPending{false};
    std::atomic_bool SuitCommitted{false};

    // rf210: set by the (anonymous-namespace) activation handler, drained by the class-scope
    // tick, which is where the party-member/identity helpers live. Also samples a frame or two
    // AFTER the click, which is what we want: an identity change is async, so this shows the
    // result of the request rather than the instant before it.
    std::atomic_bool LiveIdentityProbePending{false};

    // 0.11.4v: auto restore must call the proven F5 TogglePawn path with the
    // exact committed/saved custom suit index. In 0.11.4u the first call fell
    // back to active preview index 0 (Thomas), which spawned TheBatman2025.
    std::atomic_int64_t ForcedExactF5ConfigIndex{-1};
    std::atomic_bool SuitVisualModeActive{false};
    std::atomic_bool InjectedSelectionRefreshPending{false};
    std::atomic_uint64_t ObservedPreviewGeneration{0};


    struct SuitOperationSnapshot
    {
        StringType Kind{};
        StringType Component{};
        StringType ComponentClass{};
        int32_t MaterialSlot{0};
        UObject* ObjectValue{};

        // Preserve a stable parent/object path for transient material
        // instances that may be reclaimed after replacement.
        StringType MaterialFallbackPath{};
        bool MaterialWasDynamic{false};

        UObject* Parent{};
        FName SocketName{};
        bool BoolValue{false};
        bool BoolValueValid{false};
        bool ReinitializePose{true};
    };

    // Forward declaration used by the asset cache. The UE4SS UObject wrapper
    // in the supported 3.0.x SDK does not expose UObject::AddToRoot(), so raw
    // pointers must never be trusted across garbage-collection passes. Every
    // cache lookup is revalidated through the current UObject array instead.
    UObject* find_loaded_object(const wchar_t* path);

    struct CachedSuitAsset
    {
        StringType Path{};
        UObject* Object{};
        uint64_t ValidatedAtMs{0};
    };

    std::vector<CachedSuitAsset> CachedSuitAssets{};

    struct RuntimeMaterialSlotState
    {
        UObject* Actor{};
        UObject* Component{};
        int32_t MaterialSlot{0};

        // The first material observed before any custom operation touches this
        // actor/component/slot. Texture-parameter suits always build their MID
        // from this baseline, never from another custom suit's material.
        UObject* OriginalMaterial{};

        // Owned by the mesh component while assigned to the slot. This remains
        // a non-owning pointer and is revalidated before every reuse.
        UObject* DynamicMaterial{};
        uint64_t LastCreateAttemptSerial{0};
    };

    std::vector<RuntimeMaterialSlotState> RuntimeMaterialSlots{};

    struct SuitSlotRuntimeState
    {
        UObject* InjectedButton{};
        UObject* SourceButton{};
        UObject* SuitMenu{};
        UObject* OptionsGrid{};
        UObject* PlayerController{};
        UObject* LastPossessedGameplayPawn{};
        StringType LastPossessedGameplayPawnName{};
        StringType LastPossessedGameplayPawnClass{};
        uint64_t LastPossessedGameplayPawnMs{0};

        // rf43: gameplay worlds and possessed pawn generations are explicit
        // lifecycle boundaries. No cached actor/widget work may cross one.
        bool LevelTransitionQuiesced{false};
        uint64_t LevelTransitionGeneration{0};
        uint64_t LevelTransitionDetectedMs{0};
        uint64_t LevelTransitionReadyNotBeforeMs{0};
        uint64_t LevelTransitionLastWaitLogMs{0};
        UObject* LevelTransitionCandidatePawn{};
        UObject* LifecycleTrackedControlledPawn{};
        StringType LifecycleStableWorldKey{};
        StringType LevelTransitionPreviousWorldKey{};
        StringType LevelTransitionCandidateWorldKey{};
        StringType LevelTransitionTrigger{};

        UObject* ActiveActor{};
        UObject* ObservedPreviewActor{};
        UObject* CapturedPreviewActor{};
        int64_t PendingPreviewApplySuitIndex{-1};
        UObject* GameplayActor{};

        UObject* GameplaySnapshotActor{};
        std::vector<SuitOperationSnapshot> GameplayOperationSnapshots{};

        // On hover, customize the most recently observed live source preview
        // immediately, then remain armed for one replacement preview actor.
        // The game often creates that replacement several seconds later.
        UObject* ImmediatePreviewReuseActor{};

        bool AssetRegistryScanned{false};
        bool MaterialsApplied{false};
        bool PreviewAutoSequenceStarted{false};
        bool GameplaySequenceCompleteLogged{false};
        bool GameplayImmediateBatchAttempted{false};
        bool GameplaySnapshotCaptured{false};

        // Exact custom config currently applied to GameplaySnapshotActor.
        // This is independent from the shared source gameplay tag.
        int64_t GameplayAppliedSuitIndex{-1};

        // 0.10.71: after a suit has completed its gameplay cosmetic pass, some
        // game events such as death/respawn can reset attachment visibility or
        // meshes on the same pawn instance. This lightweight watchdog re-runs
        // the JSON operation pass only when a configured component state drifts.
        uint64_t GameplayCosmeticIntegrityNextCheckMs{0};
        uint64_t GameplayCosmeticIntegrityLastRepairMs{0};
        int32_t GameplayCosmeticIntegrityRepairCount{0};

        // 0.11.0: this repair loop is intentionally disabled for stability;
        // log that once instead of spamming every few seconds while testing.
        bool GameplayCosmeticIntegrityRepairDisabledLogged{false};

        bool PreviewHoverLatched{false};

        // 0.10.94: custom preview application is no longer started from the
        // first hover/activation pulse. A custom button must remain the active
        // hover target for a short, stable window before the preview actor is
        // captured. This avoids applying cosmetics to the previous/incorrect
        // preview pawn while the native suit menu is still swapping actors.
        bool PreviewStableHoverArmed{false};
        int64_t PreviewStableHoverSuitIndex{-1};
        UObject* PreviewStableHoverButton{};
        uint64_t PreviewStableHoverDeadlineMs{0};
        uint64_t PreviewStableHoverExpireMs{0};
        uint64_t LastPreviewStableHoverLogMs{0};
        uint64_t LastPreviewStableHoverCancelLogMs{0};
        int64_t LastPreviewStableHoverCancelSuitIndex{-1};
        uint64_t LastCustomUnhoverSkipLogMs{0};
        int64_t LastCustomUnhoverSkipSuitIndex{-1};

        // 0.10.97: direct stable-hover applies can succeed in the log but
        // still be visually overwritten by late native preview/material setup.
        // Keep reapplying the active custom suit to the same matching preview
        // actor for a few seconds while the cursor remains on that custom slot.
        bool PreviewStableDirectReinforceArmed{false};
        UObject* PreviewStableDirectReinforceActor{};
        int64_t PreviewStableDirectReinforceSuitIndex{-1};
        uint64_t PreviewStableDirectReinforceNextMs{0};
        uint64_t PreviewStableDirectReinforceExpireMs{0};
        int32_t PreviewStableDirectReinforceAttemptCount{0};

        // JSON discovery is allowed at startup, but configured external asset
        // packages must not be deserialized until a real suit menu reaches the
        // custom-slot injection path.
        bool CustomAssetLoadingUnlocked{false};

        // The gradual worker is armed only after custom buttons have been
        // successfully inserted into a valid Batman suit grid.
        bool AssetPreloadArmed{false};
        bool AssetPreloadCompleteLogged{false};
        bool AssetPreloadFinished{false};
        bool AssetPreloadTickInProgress{false};
        std::vector<StringType> AssetPreloadSkippedAssetPaths{};

        // 0.11.2: persisted custom suit selection. The selected custom suit is
        // saved by slot_id so JSON ordering changes do not break restore.
        bool SavedSelectionFileLoaded{false};
        bool SavedSelectionApplied{false};
        bool SavedSelectionValid{false};
        StringType SavedSelectedSlotId{};
        int64_t SavedSelectedSuitIndex{-1};

        // rf65: on-possession auto-apply state.
        bool PossessionAutoApplyArmed{false};
        bool PossessionAutoApplyApplied{false};
        StringType PossessionAutoApplySuitName{};

        // Separate main-menu/title saved-suit asset prime. This warms only the
        // saved suit's cosmetics/icons so 0.11.3 can mirror it onto showcase or
        // cutscene actors without waiting for gameplay/suit-menu preload.
        bool SavedSuitAssetPreloadArmed{false};
        bool SavedSuitAssetPreloadFinished{false};
        bool SavedSuitAssetPreloadCompleteLogged{false};
        int32_t SavedSuitAssetPreloadStage{0};
        int32_t SavedSuitAssetPreloadDelayFrames{180};
        int32_t SavedSuitAssetPreloadRetryRound{0};

        // 0.11.4v2: save-driven cutscene/showcase cosmetics are delayed so
        // they do not mutate actors during the menu close / main-menu return
        // transition.
        uint64_t SavedCutsceneMirrorNotBeforeMs{0};
        bool SavedCutsceneMirrorDelayLogged{false};
        int64_t SavedCutsceneMirrorDelayIndex{-1};
        bool SavedCutsceneMirrorWaitForFrontendReady{false};
        uint64_t SavedCutsceneMirrorGeneration{0};
        uint64_t SavedCutsceneMirrorReinforceUntilMs{0};

        // rf45: replacement for the old startup/main-menu global UObject
        // scanner. BlueprintInitializeAnimation identifies the exact matching
        // frontend cutscene actor, then the update tick waits one second before
        // applying the saved suit to that actor only.
        bool SavedCutsceneEventApplyPending{false};
        UObject* SavedCutsceneEventActor{nullptr};
        StringType SavedCutsceneEventActorName{};
        StringType SavedCutsceneEventActorClass{};
        int64_t SavedCutsceneEventSuitIndex{-1};
        uint64_t SavedCutsceneEventApplyNotBeforeMs{0};
        uint64_t SavedCutsceneEventDeadlineMs{0};
        uint64_t SavedCutsceneEventGeneration{0};
        int32_t SavedCutsceneEventApplyAttempts{0};
        UObject* SavedCutsceneEventLastAppliedActor{nullptr};
        StringType SavedCutsceneEventLastAppliedActorName{};
        int64_t SavedCutsceneEventLastAppliedSuitIndex{-1};
        uint64_t SavedCutsceneEventLastAppliedMs{0};

        bool InjectedSelectionRefreshLogged{false};
        bool GameplayGiveUpLogged{false};
        bool WaitingForReplacementPreview{false};

        // A committed suit may auto-apply to exactly one newly created preview
        // when the menu opens. It is not a permanent passive capture mode.
        bool CommittedReopenCaptureArmed{false};
        uint64_t CommittedReopenCaptureDeadlineMs{0};

        // Visual selection correction is dirty-driven. It never re-runs
        // SetupButton and never changes the active hover-preview identity.
        int32_t SelectionVisualGuardFrames{0};

        // rf77: countdown after a reinject; when it hits 0 we re-apply the
        // committed suit's button selection once. The immediate one-shot at
        // inject time is clobbered when the native menu asserts its own selection
        // right after, so we re-select the equipped custom suit's button after the
        // native menu has settled (keeps it highlighted on menu reopen).
        int32_t CommittedSelectDelayFrames{0};

        // rf93: time-based (not frame-countdown) deadline to force the committed
        // suit's button into its native selected/checkmark visual via BP_OnSelected
        // once the menu has settled. Time-based so it is not stalled by reinject
        // churn clearing the entry list, and gated on CommittedSuitIndex (a custom
        // suit is equipped) rather than the transient SuitCommitted bool,
        // which is false on reopen.
        uint64_t CommittedSelectDeadlineMs{0};

        // rf98: fire-once-per-(committed index, grid instance). The committed
        // button select must fire ONCE when a menu opens / a suit is committed --
        // not repeatedly. Without this, auto-inject reinject churn re-armed the
        // deadline after every successful select (deadline returns to 0 on fire),
        // so it kept re-forcing the equipped suit's button selected. When the user
        // then hovered/clicked a DIFFERENT suit, a stale churn re-fire on the old
        // committed index flashed the previously-equipped button selected mid-click
        // (user report: "selected the electric one and another before WhoLaughs").
        // We record the (index, grid) we last selected and refuse to re-arm for the
        // same pair; a genuine new commit clears AppliedIndex and a menu reopen
        // brings a new grid pointer, both of which re-enable a single clean fire.
        int64_t CommittedSelectAppliedIndex{-1};
        UObject* CommittedSelectAppliedGrid{nullptr};

        // Hover events from the same button can arrive through several BP
        // events. Only identical same-button bursts are deduplicated.
        int64_t LastCustomHoverSuitIndex{-1};

        // Preview baseline used when switching between multiple custom suits
        // sharing one native source actor.
        UObject* PreviewSnapshotActor{};
        std::vector<SuitOperationSnapshot> PreviewOperationSnapshots{};
        bool PreviewSnapshotCaptured{false};
        int64_t PreviewAppliedSuitIndex{-1};

        // Tracks that the current preview actor is already visually custom.
        // Before another custom suit is applied to the same actor, the actor
        // must be reconstructed to the native source baseline so components
        // not touched by the next suit, such as Face, Head, or NeckPeg, cannot
        // leak from the previous suit.
        bool PreviewCustomCosmeticsApplied{false};
        UObject* PreviewCustomCosmeticsActor{};
        int64_t PreviewCustomCosmeticsSuitIndex{-1};
        uint64_t PreviewCustomReconstructionCount{0};

        // 0.8.9: when the cursor leaves a custom suit or switches between
        // custom suits sharing the same native source, restore the preview
        // actor to the native baseline before the next custom batch runs.
        bool PreviewIdleBaselineResetArmed{false};
        UObject* PreviewIdleBaselineResetActor{};
        uint64_t PreviewIdleBaselineResetDeadlineMs{0};
        int64_t PreviewIdleBaselineResetPreviousSuitIndex{-1};
        int64_t PreviewIdleBaselineResetNextSuitIndex{-1};
        StringType PreviewIdleBaselineResetReason{};

        // 0.9.9: if we switch between custom suits sharing the same native
        // source actor and the current actor has custom cosmetics, do not
        // immediately apply the next suit to that same actor. The baseline
        // restore can be partial when original materials are no longer live,
        // so reusing that actor can create mixed previews.
        UObject* QuarantinedDirtyPreviewActor{};
        uint64_t QuarantinedDirtyPreviewDeadlineMs{0};
        int64_t QuarantinedDirtyPreviousSuitIndex{-1};
        int64_t QuarantinedDirtyNextSuitIndex{-1};
        StringType QuarantinedDirtyReason{};

        bool NativeSourcePreviewRestorePending{false};

        // Set by the current cosmetic batch when a required external
        // /Game/Mods asset cannot be resolved safely. Preview/gameplay should
        // stop after that one attempt instead of retrying the unsafe load.
        bool LastApplyHadUnavailableExternalAsset{false};

        uint64_t PreviewHoverLatchDeadlineMs{0};
        uint64_t PreviewCaptureDeadlineMs{0};
        uint64_t ObservedPreviewTrustDeadlineMs{0};
        uint64_t LastCustomHoverArmMs{0};

        // The game emits activation-like events while rebuilding and closing
        // the menu. Those must not be interpreted as a real native selection.
        uint64_t MenuGenerationSettlingDeadlineMs{0};
        uint64_t CustomCommitAliasGuardDeadlineMs{0};

        // A custom hover causes several generated native-button hover events.
        // Those events are not user navigation and must not cancel immediate
        // preview reuse or replacement-preview capture.
        uint64_t CustomHoverNativeNoiseGuardDeadlineMs{0};
        int64_t CustomHoverNativeNoiseGuardSuitIndex{-1};

        // rf162: session-only GameProgress RuntimeOverride pre-unlock for locked
        // native source suits. Debounced so hover/click event noise cannot run the
        // candidate probe repeatedly while the suit menu is open. If we prove the
        // source was locked before the override, we remember that exact progress
        // tag/state and restore it after hover/click handoff.
        StringType MenuSourceRuntimeOverrideLastTag{};
        bool MenuSourceRuntimeOverrideLastApplied{false};
        uint64_t MenuSourceRuntimeOverrideLastAttemptMs{0};
        StringType MenuSourceRuntimeOverrideProgressTag{};
        StringType MenuSourceRuntimeOverrideSourceTag{};
        UObject* MenuSourceRuntimeOverrideSourceButton{};
        int32_t MenuSourceRuntimeOverridePreviousState{-1};
        bool MenuSourceRuntimeOverrideTemporaryActive{false};
        bool MenuSourceRuntimeOverrideRestoreArmed{false};
        uint64_t MenuSourceRuntimeOverrideRestoreDueMs{0};
        int32_t MenuSourceRuntimeOverrideRestoreAttempts{0};
        StringType MenuSourceRuntimeOverrideRestoreReason{};

        // Manual F9 native-character experiment: after RequestIdentity returns,
        // the real pawn swap can land asynchronously. Sample the controlled pawn
        // for a few seconds, then restore any temporary RuntimeOverride unlock and
        // borrowed pawn-metadata class paths. Command-only; never menu-wired.
        bool ManualF9PostSwapObserverArmed{false};
        uint64_t ManualF9PostSwapObserverNextMs{0};
        uint64_t ManualF9PostSwapObserverUntilMs{0};
        int32_t ManualF9PostSwapObserverSampleCount{0};
        int64_t ManualF9PostSwapObserverConfigIndex{-1};
        StringType ManualF9PostSwapObserverSlotId{};
        StringType ManualF9PostSwapObserverAcceptedLabel{};
        StringType ManualF9PostSwapObserverAcceptedTargetTag{};
        StringType ManualF9PostSwapObserverExpectedGeneratedClass{};
        StringType ManualF9PostSwapObserverProgressTag{};
        int32_t ManualF9PostSwapObserverPreviousProgressState{-1};
        bool ManualF9PostSwapObserverProgressRestoreArmed{false};
        bool ManualF9PostSwapObserverMetadataRestoreArmed{false};
        bool ManualF9PostSwapObserverGeneratedObserved{false};
        bool ManualF9PostSwapObserverVanillaThomasObserved{false};
        UObject* ManualF9PostSwapObserverOriginalPawn{};
        UObject* ManualF9PostSwapObserverPartyMember{};
        StringType ManualF9PostSwapObserverLastPawn{};
        StringType ManualF9PostSwapObserverLastPawnClass{};

        // rf171: Ctrl+F8 single-donor self-bounce measurement. This observes
        // whether switching away from TheBatman2025 destroys its spawned identity
        // or merely parks it. The answer decides whether the final V2 custom->custom
        // fix needs an eviction step.
        bool SelfBounceProbeObserverArmed{false};
        uint64_t SelfBounceProbeObserverNextMs{0};
        uint64_t SelfBounceProbeObserverUntilMs{0};
        int32_t SelfBounceProbeObserverSampleCount{0};
        UObject* SelfBounceProbePartyMember{};
        UObject* SelfBounceProbeOriginalPawn{};
        StringType SelfBounceProbeBounceTag{};
        StringType SelfBounceProbeDonorTag{};
        StringType SelfBounceProbeOriginalPawnName{};
        StringType SelfBounceProbeOriginalPawnClass{};
        StringType SelfBounceProbeLastPawnName{};
        StringType SelfBounceProbeLastPawnClass{};
        bool SelfBounceProbeDonorEverFalse{false};
        bool SelfBounceProbeDonorEverTrue{false};
        bool SelfBounceProbeBounceEverTrue{false};
        StringType NativeSelfBounceResetTag{};
        StringType NativeSelfBounceResetPawnName{};
        StringType NativeSelfBounceResetPawnClass{};

        // 0.10.80: scrolling emits duplicate native hover events in pairs.
        // Keep the behavior, but avoid hammering the log and state machine
        // while rapidly moving through the grid after hidden-suit selection.
        uint64_t LastNativeHoverCancelLogMs{0};

        // UpdateTextOnNewSelection controls the character/suit headings, while
        // the costume description is stored in a separate nested text widget.
        UObject* CharacterMainMenuTextOwner{};
        UObject* CharacterSuitMenuTextOwner{};
        std::vector<UObject*> CharacterMenuTitleTextWidgets{};
        std::vector<UObject*> CharacterMenuDescriptionTextWidgets{};
        bool CharacterMenuTextWidgetsScanned{false};
        int64_t PendingMenuTextSuitIndex{-1};
        int32_t PendingMenuTextApplyFrames{0};
        int64_t PendingSubMenuTextSuitIndex{-1};
        int32_t PendingSubMenuTextApplyFrames{0};

        int64_t LastMenuTextOverrideSuitIndex{-1};
        uint64_t LastMenuTextOverrideLogMs{0};
        int64_t LastSubMenuTextOverrideSuitIndex{-1};
        uint64_t LastSubMenuTextOverrideLogMs{0};
        int64_t LastDirectMenuTextApplySuitIndex{-1};
        uint64_t LastDirectMenuTextApplyLogMs{0};
        int64_t LastDirectSubMenuTextApplySuitIndex{-1};
        uint64_t LastDirectSubMenuTextApplyLogMs{0};

        // SetButtonText is intercepted directly. UpdateButtonText has no
        // useful input text to replace, so it arms a short next-update guard
        // that reapplies the JSON display_name after the native refresh.
        int32_t CustomButtonTextGuardFrames{0};
        uint64_t LastCustomButtonTextLogMs{0};

        int32_t ExactMaterialNextStage{0};
        uint8_t ExactMaterialAttemptMask{0};
        uint8_t ExactMaterialSuccessMask{0};

        int32_t GameplayNextStage{0};
        uint8_t GameplayAttemptMask{0};
        uint8_t GameplaySuccessMask{0};
        int32_t GameplayStablePollCount{0};
        int32_t GameplayApplyRetryCount{0};
        int32_t PreviewApplyRetryCount{0};
        int32_t AssetPreloadStage{0};
        int32_t AssetPreloadDelayFrames{2};
        int32_t AssetPreloadRetryRound{0};
        int32_t InjectedSelectionRefreshFrames{0};

        int32_t GameplayStageCooldownFrames{0};

        uint64_t AssetLoadAttemptCount{0};
        uint64_t GameplayApplyCount{0};
        uint64_t CommitCount{0};
        uint64_t RestoreCount{0};

        bool SuitLoadedNotificationShown{false};
        bool SuitLoadedNotificationHideArmed{false};
        bool SuitLoadedNotificationSearchLogged{false};
        bool SuitLoadedNotificationMissingLogged{false};
        uint64_t SuitLoadedNotificationHideDeadlineMs{0};
        uint64_t SuitLoadedNotificationNextSearchMs{0};
        uint64_t SuitLoadedNotificationNextProgressMs{0};
        int32_t SuitLoadedNotificationLastStage{-1};
        UObject* SuitLoadedNotificationWidget{};

        UObject* LastInjectedOptionsGrid{};

        // rf206: log the native-button icon re-assert once per generation, not every 150ms.
        bool NativeBridgeIconReassertLogged{false};

        // rf212: one-shot guard for the mod-pak config-override probe.
        bool PakOverrideProbeLogged{false};
        int32_t LastInjectedBaseSlots{-1};
        int32_t LastInjectedBaseButtons{-1};
        int32_t LastInjectedCount{0};

        // rf71: auto-inject-on-menu-open debounce. AutoInjectLastGrid is the live
        // grid we last auto-injected into; we only auto-inject again when a new/
        // different open grid appears (menu reopened), never per-tick.
        UObject* AutoInjectLastGrid{};
        uint64_t AutoInjectNextCheckMs{0};

        // rf85: grid + button count seen on the previous auto-inject probe. We
        // only inject once the same grid reports the same count twice in a row, so
        // a still-populating (partial) grid is not injected at the wrong end.
        UObject* AutoInjectStabilityGrid{};
        int32_t AutoInjectStabilityButtons{-1};
        uint64_t LastGridInjectionSkipLogMs{0};
        uint64_t SuitGridInjectionSuppressUntilMs{0};
        bool SuitGridInjectionSuppressLogged{false};

        // rf237: one-shot, read-only census of the next stable native Batman
        // suit grid. The command marks the observed grid as already handled by
        // auto-injection, so that one menu generation remains entirely native.
        // Closing the menu clears AutoInjectLastGrid through the existing rf71
        // lifecycle and restores normal injection on the next open.
        bool NativeSuitMenuCensusArmed{false};
        uint64_t NativeSuitMenuCensusSequence{0};
        UObject* NativeSuitMenuCensusStabilityGrid{};
        int32_t NativeSuitMenuCensusStabilityButtons{-1};
        int32_t NativeSuitMenuCensusStableSamples{0};

        // 0.11.4: saved custom suits should request the native party system to
        // hand the player to the configured source pawn (for Electric, Rainbow)
        // instead of visually painting the wrong spawned pawn (for example
        // Vampire). This is intentionally small and retry-limited.
        int64_t PartyTargetHandoffSuitIndex{-1};
        UObject* PartyTargetHandoffObservedPawn{};
        int32_t PartyTargetHandoffAttempts{0};
        uint64_t PartyTargetHandoffNextAttemptMs{0};
        bool PartyTargetHandoffComplete{false};
        bool PartyTargetHandoffGeneratedTargetComplete{false};
        int64_t PartyTargetHandoffGeneratedTargetSuitIndex{-1};
        bool PartyTargetHandoffGeneratedTargetLogged{false};
        bool PartyTargetHandoffGiveUpLogged{false};
        bool PartyTargetHandoffMenuSuppressLogged{false};

        // 0.10.81: the suit menu can transiently rebuild its grid while the
        // player scrolls. Reinjecting another full set of cloned buttons into
        // that rebuild path is unsafe, so treat a menu open as a single custom
        // injection session until the native grid has disappeared for a real
        // close/reopen window.
        bool SuitGridInjectedThisOpen{false};
        uint64_t SuitGridMissingSinceMs{0};
        uint64_t LastSuccessfulGridInjectionMs{0};
        uint64_t LastSessionReinjectSkipLogMs{0};

        // rf5: native-hover reinjection helper. This is intentionally only
        // used to force the active visible menu grid to inject/reinject. It
        // does not alter gameplay apply or source-pawn handoff behavior.
        bool NativeBatmanHoverInjectionAttemptedThisOpen{false};
        uint64_t LastNativeBatmanHoverInjectionLogMs{0};

        bool PostCustomMainMenuCutsceneApplyPending{false};
        int64_t PostCustomMainMenuCutsceneApplyIndex{-1};
        uint64_t PostCustomMainMenuCutsceneNextProbeMs{0};
        uint64_t PostCustomMainMenuCutsceneLastLogMs{0};

        bool HudWarningTextDiscoveryRan{false};
        uint64_t HudWarningTextDiscoveryNextLogMs{0};

        // 0.10.20 EquipmentContainer guarded swap. Variable names kept from guarded-swap branch.
        bool EquipmentContainerDryRunPending{false};
        bool EquipmentContainerDryRunComplete{false};
        bool EquipmentContainerDryRunNoRulesLogged{false};
        int64_t EquipmentContainerDryRunSuitIndex{-1};
        int32_t EquipmentContainerDryRunRetryCount{0};
        uint64_t EquipmentContainerDryRunNextAttemptMs{0};
        size_t EquipmentContainerNextRuleIndex{0};
        // rf104: was 900ms. The launch equipment swap could fire before the
        // equipment system finished settling after the gameplay-pipeline-complete
        // signal; give it more time on launch apply (user-requested "wait a little
        // longer"). Paired with the rf104 active-slot-first swap fix.
        uint64_t EquipmentContainerInitialDelayMs{2500};
        uint64_t EquipmentContainerBetweenRuleDelayMs{1600};

        // 0.10.44: In two-player Catwoman reference tests, arming the
        // equipment mutation immediately on suit-menu commit can overlap with
        // the gameplay cosmetic pipeline while the pawn is still being
        // established. Defer slot mutation until the Thomas visual pipeline
        // has fully completed on the gameplay pawn, but then process the
        // replacement rules quickly so player/menu switching cannot cancel
        // the second slot before FoamGun is installed.
        bool EquipmentContainerArmAfterGameplayPending{false};
        int64_t EquipmentContainerArmAfterGameplaySuitIndex{-1};
        uint64_t EquipmentContainerArmAfterGameplayQueuedMs{0};

        // rf125: the equipment swap may only run after the cosmetic suit pipeline
        // has COMPLETED for this exact controlled pawn (recorded here) and the
        // materials have warmed -- so a swap armed in one level never fires on a
        // transitioning/cold pawn. ChangeEquipmentInSlot against a cold cooked
        // shader map on a streaming/background thread triggers the uncatchable
        // MaterialShared.cpp:740 fatal (the Batcave->Gotham crash). Cleared on
        // level-transition quiesce so the new world must re-confirm cosmetics.
        UObject* EquipmentSwapCosmeticReadyPawn{nullptr};
        uint64_t EquipmentSwapCosmeticReadyMs{0};

        bool EquipmentSwitchMonitorActive{false};
        uint64_t EquipmentSwitchMonitorDeadlineMs{0};
        uint64_t EquipmentSwitchMonitorNextSampleMs{0};
        int32_t EquipmentSwitchMonitorLastActiveSlot{-999};
        StringType EquipmentSwitchMonitorLastItem{};

        // 0.10.74: Minimal FoamGun visual prime. This is intentionally not
        // the old recovery loop: no K2_OnEquipped, no SetBeingUsed, no GAS.
        // It only makes already-spawned FoamGun actors/components visible
        // while slot 1 is active, to avoid the "first click primes, second
        // click shows model" issue.
        bool FoamGunVisualPrimeActive{false};
        uint64_t FoamGunVisualPrimeDeadlineMs{0};
        uint64_t FoamGunVisualPrimeNextMs{0};
        int32_t FoamGunVisualPrimeForceCount{0};
        StringType FoamGunVisualPrimeLastItem{};

        // 0.10.76: safe FoamGun first-right-click helper. This does not scan
        // spawned actors/components and does not call SetBeingUsed or GAS.
        // It only reacts when the player actively switches to slot 1, then
        // replays the native active-slot setup and unhides/resets the item.
        bool FoamGunSlotSwitchPrimeActive{false};
        uint64_t FoamGunSlotSwitchPrimeDeadlineMs{0};
        uint64_t FoamGunSlotSwitchPrimeNextSampleMs{0};
        int32_t FoamGunSlotSwitchPrimeLastActiveSlot{-999};
        StringType FoamGunSlotSwitchPrimeLastItem{};
        int32_t FoamGunSlotSwitchPrimeCount{0};

        int32_t FoamGunLifecycleBadSampleCount{0};
        bool FoamGunLifecycleGoodStateSeen{false};
        bool FoamGunLifecycleRecoveryAttempted{false};
        uint64_t FoamGunLifecycleLastGoodStateMs{0};

        int32_t ActiveEquipmentRecoveryBadSampleCount{0};
        bool ActiveEquipmentRecoveryGoodStateSeen{false};
        uint64_t ActiveEquipmentRecoveryLastGoodStateMs{0};
        uint64_t ActiveEquipmentRecoveryLastAttemptMs{0};
        int32_t ActiveEquipmentRecoveryAttemptCount{0};
        StringType ActiveEquipmentRecoveryLastItem{};
        int32_t ActiveEquipmentRecoveryLastSlot{-999};
        StringType ActiveEquipmentRecoveryLastKind{};

        bool AbilityInputDiscoveryLogged{false};
        bool AbilityObjectScanLogged{false};
        StringType AbilityInputDiscoveryLastPawn{};

        bool AbilitySpecPropertyDumpLogged{false};
        uint64_t LastGasInputReleasedMs{0};
        StringType LastGasInputReleasedFunction{};
        StringType LastGasInputReleasedValue{};
        uint64_t LastReleaseAwareCleanupMs{0};

        // 0.10.60: Whip input relay remains observation-only. 0.10.59 proved the native item reaches being_used=true, so this build adds a visual mesh bridge that rides the spawned BP_Whip_C actor.
        // The direct Catwoman parity activation at suit-commit time proved
        // risky and also did not prove the player's real input handle was
        // mapped to the swapped whip. These fields arm a short relay window
        // after the clean DA_ETA_Whip -> BP_Whip_ED_C swap, then only react
        // when the player's Batman ability system emits a real GAS input
        // release event.
        bool WhipInputRelayArmed{false};
        bool WhipInputRelayPending{false};
        UObject* WhipInputRelayAbilitySystem{};
        uint64_t WhipInputRelayDeadlineMs{0};
        uint64_t WhipInputRelayNextAttemptMs{0};
        int32_t WhipInputRelayAttemptCount{0};
        StringType WhipInputRelayLastContext{};
        StringType WhipInputRelayLastFunction{};
        StringType WhipInputRelayLastHandle{};

        // 0.10.71: FoamGun slot-1 pivot plus generic gameplay cosmetic integrity repair.
        // Whip remains hard, so this build moves Thomas slot 1 to FoamGun and
        // watches completed gameplay cosmetics for component/mesh/visibility resets
        // after death/respawn. The repair path is JSON-driven and works for any suit
        // with hair, helmet, cowl, torso, or other component operations.
        bool WhipSourceObjectProbeArmed{false};
        bool WhipSourceObjectProbeFunctionMatrixLogged{false};
        bool WhipSourceObjectProbeItemContextLogged{false};
        uint64_t WhipSourceObjectProbeDeadlineMs{0};
        uint64_t WhipSourceObjectProbeNextMs{0};
        int32_t WhipSourceObjectProbeDumpCount{0};
        UObject* WhipSourceObjectProbePawn{};
        UObject* WhipSourceObjectProbeItem{};
        StringType WhipSourceObjectProbeReason{};

        // 0.10.44 delayed FoamGun priming window.
        uint64_t FoamGunDelayedPrimeUntilMs{0};
        uint64_t FoamGunDelayedPrimeNextMs{0};
        int32_t FoamGunDelayedPrimeCount{0};
        StringType FoamGunDelayedPrimeItem{};

        // 0.10.44: reference debug for a real Catwoman partner/pawn.
        bool CatwomanReferenceDebugArmed{false};
        uint64_t CatwomanReferenceDebugDeadlineMs{0};
        uint64_t CatwomanReferenceDebugNextMs{0};
        int32_t CatwomanReferenceDebugDumpCount{0};
        StringType CatwomanReferenceDebugLastContext{};
        UObject* CatwomanReferenceDebugAbilitySystemContext{};
        bool CatwomanReferenceDebugContextSpecLogged{false};

        // 0.10.68: one-shot driver/property comparison for the live whip
        // ability instances themselves. 0.10.67 proved SourceObject is
        // present on Thomas, so this looks for the missing animation/visual
        // driver state inside GA_Whip*/GA_Item_Whip* instances.
        bool WhipAbilityInstanceProbeLogged{false};
        bool CatwomanWhipAbilityInstanceProbeLogged{false};

    };

    SuitSlotRuntimeState SuitSlotState{};

    // 0.10.52: delayed Whip visual clone state.
    // Kept outside the gameplay cosmetic pipeline because JSON-time visual
    // attachment was crashy. This only runs after the clean ED equipment swap
    // has completed and only uses already-loaded Catwoman visual assets.
    bool DeferredWhipVisualClonePending{false};
    bool DeferredWhipVisualCloneDone{false};
    UObject* DeferredWhipVisualClonePawn{nullptr};
    UObject* DeferredWhipVisualCloneComponent{nullptr};
    uint64_t DeferredWhipVisualCloneNextAttemptMs{0};
    uint64_t DeferredWhipVisualCloneLifecycleNextMs{0};
    bool DeferredWhipVisualCloneVisible{false};
    int32_t DeferredWhipVisualCloneAttempts{0};

    bool contains(StringViewType haystack, StringViewType needle)
    {
        return haystack.find(needle) != StringViewType::npos;
    }

    bool starts_with(StringViewType text, StringViewType prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }

    // Gameplay tags and manifest identities are authored as ASCII names, but
    // shipped content is not casing-consistent (notably CatWoman vs Catwoman).
    // Preserve the authored spelling for serialization/logs while matching tag
    // identity case-insensitively everywhere it crosses a saved/runtime boundary.
    bool tag_identity_equals(StringViewType left, StringViewType right)
    {
        return left.size() == right.size() &&
            std::equal(
                left.begin(),
                left.end(),
                right.begin(),
                [](const auto left_character, const auto right_character)
                {
                    return std::towlower(left_character) ==
                        std::towlower(right_character);
                });
    }


    bool tag_identity_starts_with(
        StringViewType text,
        StringViewType prefix)
    {
        return text.size() >= prefix.size() &&
            std::equal(
                prefix.begin(),
                prefix.end(),
                text.begin(),
                [](const auto prefix_character, const auto text_character)
                {
                    return std::towlower(prefix_character) ==
                        std::towlower(text_character);
                });
    }


    bool is_tag_identity_character(const wchar_t character)
    {
        return std::iswalnum(character) != 0 ||
            character == L'_' ||
            character == L'.';
    }


    // ExportText serializes gameplay tags inside larger map/struct strings. Match
    // the tag case-insensitively, but require token boundaries so a saved
    // `...Electric` identity cannot accidentally confirm `...ElectricV2`.
    bool exported_text_contains_tag_identity(
        StringViewType exported_text,
        StringViewType pawn_tag)
    {
        if (exported_text.empty() || pawn_tag.empty() ||
            exported_text.size() < pawn_tag.size())
        {
            return false;
        }

        auto cursor = exported_text.begin();
        while (cursor != exported_text.end())
        {
            const auto match = std::search(
                cursor,
                exported_text.end(),
                pawn_tag.begin(),
                pawn_tag.end(),
                [](const auto left_character, const auto right_character)
                {
                    return std::towlower(left_character) ==
                        std::towlower(right_character);
                });
            if (match == exported_text.end())
            {
                return false;
            }

            const auto match_offset = static_cast<size_t>(
                std::distance(exported_text.begin(), match));
            const auto match_end_offset = match_offset + pawn_tag.size();
            const bool left_boundary =
                match_offset == 0 ||
                !is_tag_identity_character(exported_text[match_offset - 1]);
            const bool right_boundary =
                match_end_offset == exported_text.size() ||
                !is_tag_identity_character(exported_text[match_end_offset]);
            if (left_boundary && right_boundary)
            {
                return true;
            }

            cursor = match + 1;
        }

        return false;
    }


    std::filesystem::path lotdk_expanded_root()
    {
        std::array<wchar_t, 32768> executable_path{};
        const DWORD length = GetModuleFileNameW(
            nullptr,
            executable_path.data(),
            static_cast<DWORD>(executable_path.size()));

        if (length > 0 && length < executable_path.size())
        {
            const auto executable_directory =
                std::filesystem::path{executable_path.data()}.parent_path();

            return (executable_directory /
                    L"ue4ss" /
                    L"LOTDKExpanded").lexically_normal();
        }

        std::error_code error{};
        const auto current = std::filesystem::current_path(error);
        if (!error)
        {
            if (current.filename() == L"ue4ss")
            {
                return (current / L"LOTDKExpanded").lexically_normal();
            }

            return (current / L"ue4ss" / L"LOTDKExpanded").lexically_normal();
        }

        return std::filesystem::path{L"ue4ss"} / L"LOTDKExpanded";
    }

    std::vector<std::filesystem::path> native_suit_mod_discovery_roots()
    {
        return { (lotdk_expanded_root() / L"Mods").lexically_normal() };
    }


    bool class_name_contains(UObject* object, StringViewType class_path)
    {
        auto* object_class = object ? object->GetClassPrivate() : nullptr;
        const auto class_name = object_class ? object_class->GetFullName() : STR("");
        return contains(class_name, class_path);
    }

    StringType widen_utf8(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0);

        if (required <= 0)
        {
            StringType fallback{};
            fallback.reserve(text.size());
            for (const unsigned char character : text)
            {
                fallback.push_back(static_cast<wchar_t>(character));
            }
            return fallback;
        }

        StringType result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            required);
        return result;
    }

    std::string narrow_utf8(StringViewType text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (required <= 0)
        {
            std::string fallback{};
            fallback.reserve(text.size());
            for (const auto character : text)
            {
                fallback.push_back(
                    character >= 0 && character <= 0x7F
                        ? static_cast<char>(character)
                        : '?');
            }
            return fallback;
        }

        std::string result(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            required,
            nullptr,
            nullptr);

        return result;
    }

    std::string json_escape_string(const std::string& value)
    {
        std::string escaped{};
        escaped.reserve(value.size());

        for (const char character : value)
        {
            switch (character)
            {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(character);
                break;
            }
        }

        return escaped;
    }

    std::string read_text_file(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }

        return std::string{
            std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}};
    }

    bool json_find_value_start(
        const std::string& object_text,
        const char* key,
        size_t& out_cursor)
    {
        const std::string quoted_key =
            std::string{"\""} + key + "\"";

        const auto key_pos = object_text.find(quoted_key);
        if (key_pos == std::string::npos)
        {
            return false;
        }

        const auto colon_pos =
            object_text.find(':', key_pos + quoted_key.size());

        if (colon_pos == std::string::npos)
        {
            return false;
        }

        out_cursor = colon_pos + 1;
        while (out_cursor < object_text.size() &&
               std::isspace(
                   static_cast<unsigned char>(
                       object_text[out_cursor])))
        {
            ++out_cursor;
        }

        return out_cursor < object_text.size();
    }

    bool json_find_string_value(
        const std::string& object_text,
        const char* key,
        std::string& value)
    {
        size_t cursor = 0;
        if (!json_find_value_start(object_text, key, cursor) ||
            object_text[cursor] != '"')
        {
            return false;
        }

        ++cursor;
        std::string parsed{};
        bool escaped = false;

        for (; cursor < object_text.size(); ++cursor)
        {
            const auto character = object_text[cursor];

            if (escaped)
            {
                switch (character)
                {
                case '"':
                case '\\':
                case '/':
                    parsed.push_back(character);
                    break;
                case 'n':
                    parsed.push_back('\n');
                    break;
                case 'r':
                    parsed.push_back('\r');
                    break;
                case 't':
                    parsed.push_back('\t');
                    break;
                default:
                    parsed.push_back(character);
                    break;
                }

                escaped = false;
                continue;
            }

            if (character == '\\')
            {
                escaped = true;
                continue;
            }

            if (character == '"')
            {
                value = std::move(parsed);
                return true;
            }

            parsed.push_back(character);
        }

        return false;
    }

    bool json_find_bool_value(
        const std::string& object_text,
        const char* key,
        bool& value)
    {
        size_t cursor = 0;
        if (!json_find_value_start(object_text, key, cursor))
        {
            return false;
        }

        if (object_text.compare(cursor, 4, "true") == 0)
        {
            value = true;
            return true;
        }

        if (object_text.compare(cursor, 5, "false") == 0)
        {
            value = false;
            return true;
        }

        return false;
    }

    bool json_find_int_value(
        const std::string& object_text,
        const char* key,
        int32_t& value)
    {
        size_t cursor = 0;
        if (!json_find_value_start(object_text, key, cursor))
        {
            return false;
        }

        const auto begin = cursor;
        if (cursor < object_text.size() &&
            (object_text[cursor] == '-' ||
             object_text[cursor] == '+'))
        {
            ++cursor;
        }

        while (cursor < object_text.size() &&
               std::isdigit(
                   static_cast<unsigned char>(
                       object_text[cursor])))
        {
            ++cursor;
        }

        if (cursor == begin)
        {
            return false;
        }

        try
        {
            value = std::stoi(
                object_text.substr(begin, cursor - begin));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool json_find_named_array_text(
        const std::string& object_text,
        const char* key,
        std::string& array_text)
    {
        size_t cursor = 0;
        if (!json_find_value_start(object_text, key, cursor) ||
            object_text[cursor] != '[')
        {
            return false;
        }

        const auto start = cursor;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;

        for (; cursor < object_text.size(); ++cursor)
        {
            const auto character = object_text[cursor];

            if (in_string)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if (character == '"')
                {
                    in_string = false;
                }
                continue;
            }

            if (character == '"')
            {
                in_string = true;
                continue;
            }

            if (character == '[')
            {
                ++depth;
            }
            else if (character == ']')
            {
                --depth;
                if (depth == 0)
                {
                    array_text =
                        object_text.substr(
                            start,
                            cursor - start + 1);
                    return true;
                }
            }
        }

        return false;
    }

    bool json_find_named_object_text(
        const std::string& object_text,
        const char* key,
        std::string& nested_object_text)
    {
        size_t cursor = 0;
        if (!json_find_value_start(object_text, key, cursor) ||
            object_text[cursor] != '{')
        {
            return false;
        }

        const auto start = cursor;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;

        for (; cursor < object_text.size(); ++cursor)
        {
            const auto character = object_text[cursor];
            if (in_string)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if (character == '"')
                {
                    in_string = false;
                }
                continue;
            }

            if (character == '"')
            {
                in_string = true;
                continue;
            }
            if (character == '{')
            {
                ++depth;
            }
            else if (character == '}')
            {
                --depth;
                if (depth == 0)
                {
                    nested_object_text = object_text.substr(
                        start,
                        cursor - start + 1);
                    return true;
                }
            }
        }

        return false;
    }

    std::vector<std::string> json_objects_in_array_text(
        const std::string& array_text)
    {
        std::vector<std::string> objects{};
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        size_t object_start = std::string::npos;

        for (size_t cursor = 0;
             cursor < array_text.size();
             ++cursor)
        {
            const auto character = array_text[cursor];

            if (in_string)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if (character == '"')
                {
                    in_string = false;
                }
                continue;
            }

            if (character == '"')
            {
                in_string = true;
                continue;
            }

            if (character == '{')
            {
                if (depth == 0)
                {
                    object_start = cursor;
                }
                ++depth;
            }
            else if (character == '}')
            {
                --depth;
                if (depth == 0 &&
                    object_start != std::string::npos)
                {
                    objects.emplace_back(
                        array_text.substr(
                            object_start,
                            cursor - object_start + 1));
                    object_start = std::string::npos;
                }
            }
        }

        return objects;
    }

    StringType canonical_object_path(StringType path)
    {
        if (path.empty())
        {
            return path;
        }

        const auto slash = path.find_last_of(STR("/"));
        const auto dot = path.find_last_of(STR("."));

        if (dot != StringType::npos &&
            (slash == StringType::npos || dot > slash))
        {
            return path;
        }

        const auto name_start =
            slash == StringType::npos ? 0 : slash + 1;

        if (name_start >= path.size())
        {
            return path;
        }

        return path + STR(".") + path.substr(name_start);
    }

    bool ends_with(StringViewType text, StringViewType suffix)
    {
        return text.size() >= suffix.size() &&
            text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    StringType canonical_blueprint_generated_class_path(StringType path)
    {
        path = canonical_object_path(path);

        if (path.empty() ||
            ends_with(path, STR("_C")))
        {
            return path;
        }

        const auto slash = path.find_last_of(STR("/"));
        const auto dot = path.find_last_of(STR("."));
        if (dot == StringType::npos ||
            (slash != StringType::npos && dot < slash))
        {
            return path;
        }

        return path + STR("_C");
    }

    struct SuitOperation
    {
        StringType Type{};
        StringType Component{};
        StringType ComponentClass{};
        StringType Parent{};
        StringType Socket{};
        StringType Asset{};
        StringType Material{};
        StringType AnimClass{};
        std::vector<StringType> Materials{};

        // Used by material parameter operations. This is the exact,
        // case-sensitive parameter name exposed by the source material.
        StringType ParameterName{};

        // Used by set_vector_parameter. Values are linear RGBA values and are
        // normally supplied in the normalized 0.0-1.0 range.
        double VectorR{0.0};
        double VectorG{0.0};
        double VectorB{0.0};
        double VectorA{1.0};
        bool VectorValueValid{false};

        // Used by set_scalar_parameter. This lets JSON change numeric material
        // instance parameters such as ColourChannel_Picker on the cowl/hat MID.
        double ScalarValue{0.0};
        bool ScalarValueValid{false};

        // Used by set_material_full: a batch of texture/vector/scalar parameter
        // overrides applied to a dynamic instance of the base material (Asset), in
        // one op. Lets a locked-source suit carry its own complete material instead
        // of tweaking the (silhouette) material already on the slot.
        struct MaterialParameterEntry
        {
            StringType Type{};      // "texture" | "vector" | "scalar"
            StringType Name{};      // exact material parameter name
            StringType Asset{};     // texture object path (Type == "texture")
            double VectorR{0.0};
            double VectorG{0.0};
            double VectorB{0.0};
            double VectorA{1.0};
            double ScalarValue{0.0};
        };
        std::vector<MaterialParameterEntry> MaterialParameters{};

        StringType ApplyTo{STR("both")};
        int32_t MaterialSlot{0};
        bool Visible{true};
        bool Hidden{true};
        bool PropagateToChildren{false};
        bool ReinitializePose{true};
        bool Required{true};
        bool Enabled{true};
    };

    struct EquipmentReplacementRule
    {
        // 0.10.19: parsed again for live EquipmentContainer guarded-swap.
        // No mutation is performed in this build.
        StringType ReplaceEquipment{};
        StringType WithEquipment{};
        StringType ReplaceUpgrade{};
        StringType WithUpgrade{};

        // slot = "current" is the default. Numeric slot is also accepted.
        StringType SlotText{STR("current")};
        int32_t Slot{-1};
        bool UseCurrentSlot{true};

        bool Required{true};
        bool Enabled{true};
    };

    struct VisualSlotJsonConfig
    {
        int32_t SchemaVersion{1};
        int32_t MenuOrder{1000};
        bool Enabled{true};
        StringType SlotId{};
        StringType DisplayName{};
        StringType Description{};
        StringType SourceTag{};
        StringType TargetPawnTag{};
        StringType ProgressTag{};
        StringType SourceActorClassContains{};

        // Native-character asset manifest fields. The bridge/source/progress
        // tags are intentionally hardcoded to TheBatman2025; these paths only
        // describe which generated classes the bridge metadata should point at
        // while a custom suit handoff is in progress.
        StringType DcmdAsset{};
        StringType PlayableBpAsset{};
        StringType PlayableClassAsset{};
        StringType MenuActorClassAsset{};
        StringType CutsceneBpAsset{};
        StringType CutsceneClassAsset{};
        StringType UIMDAsset{};

        // Button icon used by our cloned custom suit button.
        StringType IconAsset{};

        // Optional TtPawnUIMetaData texture overrides. These are separate
        // from the custom button icon and map directly onto the target suit's
        // UIMD properties. SuitIcon is intentionally not exposed here because
        // this mod already drives the custom button icon through icon_asset.
        StringType UIMDMenuIconAsset{};
        StringType UIMDRightFacingAsset{};
        StringType UIMDLeftFacingAsset{};

        std::vector<SuitOperation> Operations{};
        std::vector<EquipmentReplacementRule> EquipmentReplacements{};
        std::filesystem::path JsonPath{};
        bool LoadedFromJson{false};
        bool Valid{false};
        StringType ValidationError{};
    };

    // Schema-3 aggregate native mod manifest. This is deliberately separate
    // from VisualSlotJsonConfig: mod.json describes already-cooked native
    // characters that must be registered, while the legacy suit.json model
    // contains the old runtime bridge/operation system.
    struct NativeSuitModManifestSuit
    {
        StringType SuitId{};
        StringType PawnTag{};
        // Stable owner of this suit selection.  The builder may write this
        // explicitly; old schema-3 manifests derive it from PawnTag so they
        // remain valid without a tool migration.
        StringType CharacterScope{};
        StringType ProgressTag{};
        StringType DisplayNameKey{};
        StringType DescriptionKey{};
        StringType LockedDescriptionKey{};

        // Package paths exactly as emitted by the builder.
        StringType PlayablePackage{};
        StringType CutscenePackage{};
        StringType DcmdPackage{};

        // Normalized runtime object/class paths derived from the package paths.
        StringType PlayableClassPath{};
        StringType CutsceneClassPath{};
        StringType DcmdObjectPath{};
        StringType DcmdAssetName{};
        StringType DcmdPackagePath{};

        int32_t MenuOrder{1000};
        bool Enabled{true};
        bool Valid{false};
        StringType ValidationError{};
    };

    struct NativeSuitModManifest
    {
        int32_t SchemaVersion{0};
        StringType Format{};
        StringType ModId{};
        StringType DisplayName{};
        StringType PackageBaseName{};
        StringType ContentRoot{};
        StringType StringTable{};
        StringType BuildId{};
        std::filesystem::path JsonPath{};
        std::vector<NativeSuitModManifestSuit> Suits{};
        bool LoadedFromJson{false};
        bool Valid{false};
        StringType ValidationError{};
    };

    // A player can own one remembered custom suit per playable character.
    // Keep the on-disk schema deliberately independent of the manifest schema:
    // manifest schema 3 remains compatible with existing released suit packs.
    struct NativeManifestSavedSelection
    {
        StringType CharacterScope{};
        StringType ModId{};
        StringType SuitId{};
        StringType PawnTag{};
        // False means the row came from a well-formed save but its manifest is
        // not currently installed/valid. Keep it on disk so temporarily moving
        // or repairing another mod cannot erase an unrelated character's choice;
        // never use a dormant row for class loading or identity restoration.
        bool ManifestResolved{false};
    };

    // The frontend is Batman-only. A saved custom suit for Gordon, Nightwing,
    // or any future character must stay in the sidecar for that character, but
    // it must not cause their generated classes to load during Batman's
    // frontend startup. Their assets are instead handled when that character's
    // native menu/identity becomes relevant in gameplay.
    constexpr StringViewType NativeManifestStartupPreloadCharacterScope =
        STR("Pawns.Playable.Batman");

    StringType derive_native_manifest_character_scope(
        StringViewType pawn_tag)
    {
        constexpr StringViewType playable_prefix =
            STR("Pawns.Playable.");

        if (!tag_identity_starts_with(pawn_tag, playable_prefix) ||
            pawn_tag.size() <= playable_prefix.size())
        {
            return {};
        }

        const StringType normalized{pawn_tag};
        const auto character_end =
            normalized.find(STR("."), playable_prefix.size());

        // A fully-qualified pawn identity normally has another segment after
        // its character name (for example Pawns.Playable.Batman.Electric).
        // Accept the character tag itself too, which makes a native/base
        // button safe to use as a clear-selection scope.
        if (character_end == StringType::npos)
        {
            return normalized;
        }

        if (character_end == playable_prefix.size())
        {
            return {};
        }

        return normalized.substr(0, character_end);
    }

    StringType native_manifest_character_scope_for_suit(
        const NativeSuitModManifestSuit& suit)
    {
        return !suit.CharacterScope.empty()
            ? suit.CharacterScope
            : derive_native_manifest_character_scope(suit.PawnTag);
    }


    struct RuntimeSuitEntry
    {
        size_t ConfigIndex{0};
        UObject* InjectedButton{};
        UObject* SourceButton{};
        bool SourceButtonFallback{false};
        StringType EffectiveSourceTag{};

        std::vector<UObject*> LabelTextWidgets{};
        bool LabelTextWidgetsScanned{false};
    };


    struct RecentRuntimeSuitButtonEntry
    {
        size_t ConfigIndex{0};
        UObject* InjectedButton{};
        uint64_t ExpiresAtMs{0};
        StringType ButtonName{};
    };

    std::vector<VisualSlotJsonConfig> LoadedSuitConfigs{};
    std::vector<NativeSuitModManifest> LoadedNativeSuitModManifests{};
    bool NativeSuitModManifestsLoaded{false};
    bool NativeSchema3ManifestModeActive{false};

    // rf244: schema-3 suits are real native pawn tags, not legacy visual-slot
    // configs. Keep their selection state separate from selected_suit.json so a
    // native custom button is never misclassified as "another/native suit" and
    // erased by the retired V1 commit pipeline.
    StringType SavedNativeManifestPawnTag{};
    StringType SavedNativeManifestModId{};
    StringType SavedNativeManifestSuitId{};
    StringType SavedNativeManifestActiveCharacterScope{};
    StringType SavedNativeManifestLastActiveCharacterScope{};
    std::vector<NativeManifestSavedSelection>
        SavedNativeManifestSelections{};
    bool SavedNativeManifestSelectionsNeedMigration{false};
    bool SavedNativeManifestSelectionLoaded{false};
    // True only after the canonical sidecar was parsed as schema 2 or an atomic
    // commit to that path succeeded. File existence alone is not enough: an
    // unreadable/corrupt prior custom selection must be overwritten when the
    // player explicitly chooses a base-game suit.
    bool SavedNativeManifestCanonicalDocumentValidated{false};
    std::atomic<bool> NativeManifestPersistenceProbePending{false};
    UObject* NativeManifestLastObservedMetadataSystem{};
    UObject* NativeManifestLastObservedPreferencesSaveData{};
    StringType NativeManifestLastObservedPawnTag{};
    bool NativeManifestPreferenceHydrationMonitoringActive{false};
    uint64_t NativeManifestPreferenceHydrationStartedMs{};
    uint64_t NativeManifestPreferenceHydrationNextPollMs{};
    int32_t NativeManifestPreferenceHydrationPollCount{};
    int32_t NativeManifestPreferenceHydrationStableObservations{};
    bool NativeManifestSidecarIdentityAuditPending{false};
    uint64_t NativeManifestSidecarIdentityAuditDueMs{};
    UObject* NativeManifestSidecarIdentityAuditPartyMember{};
    UObject* NativeManifestSidecarIdentityAuditManager{};
    UObject* NativeManifestSidecarIdentityAuditMetadata{};
    StringType NativeManifestSidecarIdentityAuditPawnTag{};
    bool NativeManifestSidecarIdentityAuditAfterApply{false};
    bool NativeManifestSidecarIdentityAuditAfterClassPreload{false};

    // rf293: preload only the saved schema-3 suit's generated classes as soon
    // as its validated sidecar is read. rf292 proved that the cinematic
    // loader cannot derive GetTargetIdentityActorClass until the generated
    // cutscene class is resident. Calling TryApplyTargetIdentity after the
    // frontend sequence had already bound TheBatman2025 spawned an unbound,
    // partially initialized Electric actor and then null-wrote at game
    // RVA 0x28AD97E. The safe experiment is therefore to make both generated
    // classes resident before the frontend/gameplay identity requests occur
    // and let the game perform its normal handover. No identity request,
    // apply, spawn, possession, save write, or UObject property write occurs.
    bool NativeManifestStartupClassPreloadArmed{false};
    bool NativeManifestStartupClassPreloadAttempted{false};
    bool NativeManifestStartupClassPreloadComplete{false};
    uint64_t NativeManifestStartupClassPreloadStartedMs{};
    uint64_t NativeManifestStartupClassPreloadNextPollMs{};
    int32_t NativeManifestStartupClassPreloadPollCount{};
    StringType NativeManifestStartupClassPreloadPawnTag{};

    // Native menu previews can request a non-selected suit's generated cutscene
    // class before the game has otherwise caused that class to become resident.
    // Keep the same proven class-lifetime treatment used for the saved suit, but
    // warm every enabled schema-3 suit gradually after gameplay settles. This
    // does not request an identity, spawn an actor, change party state, or write
    // a save.
    struct NativeManifestPreviewClassWarmupEntry
    {
        StringType PawnTag{};
        StringType PlayableClassPath{};
        StringType CutsceneClassPath{};
    };

    std::vector<NativeManifestPreviewClassWarmupEntry>
        NativeManifestPreviewClassWarmupQueue{};
    bool NativeManifestPreviewClassWarmupArmed{false};
    bool NativeManifestPreviewClassWarmupComplete{false};
    bool NativeManifestPreviewClassWarmupGameplayReady{false};
    uint64_t NativeManifestPreviewClassWarmupNextPollMs{};
    uint64_t NativeManifestPreviewClassWarmupGameplayReadyMs{};
    size_t NativeManifestPreviewClassWarmupIndex{};
    int32_t NativeManifestPreviewClassWarmupAttemptsForCurrent{};
    size_t NativeManifestPreviewClassWarmupReadyCount{};
    size_t NativeManifestPreviewClassWarmupFailureCount{};

    // rf297: bounded, startup-only, read-only trace for the native decision that
    // chooses Electric versus TheBatman2025 in the frontend and DarkKnight in
    // gameplay. The earlier rf284 probe began only after exact DCMD resolution
    // and sampled every five seconds, so it could miss the decisive preference /
    // manifest / identity-manager transition. This trace begins as soon as the
    // validated schema-3 sidecar is read, samples already-live reflected state,
    // and emits only state changes (plus sparse heartbeats). It never calls a
    // UFunction and never writes a UObject, map, save, manifest, loader, pawn, or
    // controller.
    bool StartupNativeIdentityDecisionTraceArmed{false};
    bool StartupNativeIdentityDecisionTraceComplete{false};
    uint64_t StartupNativeIdentityDecisionTraceStartedMs{};
    uint64_t StartupNativeIdentityDecisionTraceNextPollMs{};
    uint64_t StartupNativeIdentityDecisionTraceLastLogMs{};
    int32_t StartupNativeIdentityDecisionTracePollCount{};
    int32_t StartupNativeIdentityDecisionTraceEventCount{};
    StringType StartupNativeIdentityDecisionTraceLastSignature{};
    StringType StartupNativeIdentityDecisionTraceTargetTag{};
    StringType StartupNativeIdentityDecisionTraceSource{};

    // Some native startup operations still enter reflection. Record only the
    // narrow party/identity handover calls while the state trace is active.
    // Native C++ direct calls may bypass this observer; the transition sampler
    // above remains the authoritative diagnostic in that case.
    std::atomic_bool StartupNativeIdentityDecisionProcessTraceArmed{false};
    std::atomic<uint64_t> StartupNativeIdentityDecisionProcessTraceDeadlineMs{0};
    std::atomic<uint64_t> StartupNativeIdentityDecisionProcessTraceHits{0};

    // rf256/rf259: cold-start native-manifest restore. The game's persistent save
    // already remembers schema-3 custom tags, but its transient party
    // preferences can fall back to a native Batman before our runtime DCMD row
    // is registered. rf245 proved that copying the saved map into the transient
    // map is insufficient and poisons the later native handover; rf253 proved
    // that a raw PartyMember.RequestIdentity is below the successful button
    // selection boundary. Keep the saved map untouched and dispatch exactly one
    // game-owned TtPartySystem.TogglePartyPawn after exact metadata registration
    // and after a real gameplay pawn/component exists. rf258 proved the narrower
    // TtPartyControllerComponent.TogglePawn wrapper can return from ProcessEvent
    // without changing identity. The party-system API owns the current pawn
    // component and is the closest reflected boundary to the real menu handover.
    bool NativeManifestStartupRestoreArmed{false};
    bool NativeManifestStartupRestoreDispatched{false};
    bool NativeManifestStartupRestoreComplete{false};
    uint64_t NativeManifestStartupRestoreStartedMs{};
    uint64_t NativeManifestStartupRestoreNextPollMs{};
    uint64_t NativeManifestStartupRestoreAuditDueMs{};
    int32_t NativeManifestStartupRestorePollCount{};
    StringType NativeManifestStartupRestorePawnTag{};
    uint64_t NativeManifestStartupRestoreGameplayObservedMs{};
    uint64_t NativeManifestStartupRestoreReadySinceMs{};
    int32_t NativeManifestStartupRestoreReadyObservations{};
    StringType NativeManifestStartupRestoreReadySignature{};

    // rf274: read-only startup construction-order probe. rf273 proved that the
    // first normal update already sees CharacterData populated and the live
    // Batman preference reduced to DarkKnight, even though both persistent save
    // maps still contain the custom Electric tag. StaticConstructObject runs
    // before each newly constructed subsystem is handed back to its caller, so
    // this probe can identify the last safe boundary before native Initialize /
    // save validation without changing a return value, asset, map, save, pawn,
    // or menu. The callback removes itself after all four core runtime systems
    // are observed (or after a bounded timeout on a later construction event).
    std::atomic_bool StartupConstructionOrderProbeInstalled{false};
    std::atomic_bool StartupConstructionOrderProbeComplete{false};
    std::atomic_uint32_t StartupConstructionOrderProbeCdoMask{0};
    std::atomic_uint32_t StartupConstructionOrderProbeRuntimeMask{0};
    std::atomic_int32_t StartupConstructionOrderProbeEventCount{0};
    uint64_t StartupConstructionOrderProbeStartedMs{};


    // Legacy deferred menu/tick UIMD queue. 0.10.90 keeps it drained/disabled
    // because even probe-only UIMD scanning during hover can crash.
    std::vector<int64_t> PendingUIMDIconOverrideIndices{};
    uint64_t PendingUIMDIconOverrideNotBeforeMs{0};
    int32_t PendingUIMDIconOverrideRetryRound{0};

    // 0.10.90: TtPawnUIMetaData mutation is a hard-disabled experiment.
    // JSON fields still parse for later direct-widget icon replacement, but no
    // metadata object is scanned or edited from hover, tick, or spawned actor.
    std::vector<int64_t> PendingSpawnTimeUIMDIconOverrideIndices{};
    std::vector<int64_t> AppliedSpawnTimeUIMDIconOverrideIndices{};

    // 0.10.92: HUD portrait replacement is live-widget-only and delayed until equipment settle.
    // It waits for live transient gameplay HUD widgets and calls W_PlayerPortrait_C::SetCharacterPortrait after equipment settle.
    int64_t PendingHudPlayerPortraitSuitIndex{-1};
    UObject* PendingHudPlayerPortraitActor{};
    uint64_t PendingHudPlayerPortraitNextApplyMs{0};
    int32_t PendingHudPlayerPortraitAttempts{0};
    int32_t PendingHudPlayerPortraitSuccesses{0};
    bool HudPlayerPortraitNativeRepairPending{false};
    int64_t HudPlayerPortraitNativeRepairSuitIndex{-1};
    uint64_t HudPlayerPortraitNativeRepairNotBeforeMs{0};
    int32_t HudPlayerPortraitNativeRepairNoPawnRetries{0};
    StringType HudPlayerPortraitNativeRepairIncomingTexture{};

    std::vector<RuntimeSuitEntry> RuntimeSuitEntries{};
    std::vector<RecentRuntimeSuitButtonEntry> RecentRuntimeSuitButtons{};
    uint64_t LastRecentRuntimeSuitButtonHitLogMs{0};

    // Incremented once for each complete cosmetic mutation pass. Dynamic
    // material creation is attempted at most once per actor/component/slot in
    // one pass, even when five texture parameters target the same slot.
    uint64_t CurrentCosmeticApplySerial{0};

    // Custom Asset Registry directories are force-scanned only once per game
    // session, after the suit-menu injection path has unlocked external loads.
    std::vector<StringType> ScannedExternalAssetDirectories{};

    struct ExternalTexturePrimeRecord
    {
        StringType Path{};
        bool Attempted{false};
        bool Loaded{false};
    };

    // Texture/icon Kismet priming is intentionally one-shot per path. Failed
    // paths may be looked up again later, but are never repeatedly passed to
    // LoadAsset_Blocking during hover or retry loops.
    std::vector<ExternalTexturePrimeRecord> ExternalTexturePrimeRecords{};

    std::vector<StringType> EquipmentDiagnosticLoggedKeys{};
    std::vector<StringType> EquipmentDiagnosticScanReasons{};

    struct RuntimeCreatedComponentAlias
    {
        UObject* Actor{};
        StringType Name{};
        UObject* Component{};
    };

    std::vector<RuntimeCreatedComponentAlias> RuntimeCreatedComponentAliases{};

    bool SuitConfigsLoaded{false};

    std::atomic_int64_t ActivePreviewSuitIndex{0};
    std::atomic_int64_t CommittedSuitIndex{-1};
    std::atomic_int64_t PendingCustomSuitIndex{-1};

    // Exact JSON definition currently responsible for the visible character
    // menu text. Native button hover/selection sets this back to -1.
    std::atomic_int64_t HoveredCustomSuitTextIndex{-1};

    void append_electric_registered_tag_button_test_config_if_missing();

    void append_native_manifest_validation_error(
        StringType& destination,
        StringViewType error)
    {
        if (error.empty())
        {
            return;
        }

        if (!destination.empty())
        {
            destination += STR("|");
        }
        destination += error;
    }

    std::vector<std::filesystem::path> find_native_suit_mod_json_files()
    {
        std::vector<std::filesystem::path> files{};

        for (const auto& root : native_suit_mod_discovery_roots())
        {
            std::error_code error{};
            const bool usable =
                std::filesystem::is_directory(root, error);

            suit_vlog(
                STR("{} SuitRuntime NATIVE_MOD_MANIFEST_DISCOVERY_ROOT root={} usable={} policy=schema3-log-only-no-registration\n"),
                ModTag,
                root.wstring(),
                usable && !error ? STR("yes") : STR("no"));

            if (!usable || error)
            {
                continue;
            }

            error.clear();
            for (const auto& entry :
                 std::filesystem::directory_iterator{root, error})
            {
                if (error)
                {
                    break;
                }

                std::error_code entry_error{};
                if (!entry.is_directory(entry_error) ||
                    entry_error)
                {
                    continue;
                }

                const auto manifest_path =
                    entry.path() / L"mod.json";

                entry_error.clear();
                if (std::filesystem::is_regular_file(
                        manifest_path,
                        entry_error) &&
                    !entry_error)
                {
                    files.push_back(
                        manifest_path.lexically_normal());
                }
            }
        }

        std::sort(files.begin(), files.end());
        files.erase(
            std::unique(files.begin(), files.end()),
            files.end());
        return files;
    }

    NativeSuitModManifest parse_native_suit_mod_manifest(
        const std::filesystem::path& path)
    {
        NativeSuitModManifest manifest{};
        manifest.JsonPath = path;

        const auto contents =
            read_text_file(path);
        if (contents.empty())
        {
            manifest.ValidationError =
                STR("empty-or-unreadable-file");
            return manifest;
        }

        manifest.LoadedFromJson = true;

        const auto assign_manifest_string =
            [&](const char* key, StringType& destination)
            {
                std::string value{};
                if (json_find_string_value(
                        contents,
                        key,
                        value))
                {
                    destination =
                        widen_utf8(value);
                }
            };

        json_find_int_value(
            contents,
            "schema_version",
            manifest.SchemaVersion);
        assign_manifest_string(
            "format",
            manifest.Format);
        assign_manifest_string(
            "mod_id",
            manifest.ModId);
        assign_manifest_string(
            "display_name",
            manifest.DisplayName);
        assign_manifest_string(
            "package_base_name",
            manifest.PackageBaseName);
        assign_manifest_string(
            "content_root",
            manifest.ContentRoot);
        assign_manifest_string(
            "string_table",
            manifest.StringTable);
        assign_manifest_string(
            "build_id",
            manifest.BuildId);

        if (!manifest.StringTable.empty())
        {
            manifest.StringTable =
                canonical_object_path(
                    manifest.StringTable);
        }

        std::string suits_array_text{};
        const bool has_suits_array = json_find_named_array_text(
                contents,
                "suits",
                suits_array_text);
        if (has_suits_array)
        {
            for (const auto& suit_text :
                 json_objects_in_array_text(
                     suits_array_text))
            {
                NativeSuitModManifestSuit suit{};

                const auto assign_suit_string =
                    [&](const char* key, StringType& destination)
                    {
                        std::string value{};
                        if (json_find_string_value(
                                suit_text,
                                key,
                                value))
                        {
                            destination =
                                widen_utf8(value);
                        }
                    };

                json_find_bool_value(
                    suit_text,
                    "enabled",
                    suit.Enabled);
                json_find_int_value(
                    suit_text,
                    "menu_order",
                    suit.MenuOrder);

                assign_suit_string(
                    "suit_id",
                    suit.SuitId);
                assign_suit_string(
                    "pawn_tag",
                    suit.PawnTag);
                assign_suit_string(
                    "character_scope",
                    suit.CharacterScope);
                assign_suit_string(
                    "progress_tag",
                    suit.ProgressTag);
                assign_suit_string(
                    "display_name_key",
                    suit.DisplayNameKey);
                assign_suit_string(
                    "description_key",
                    suit.DescriptionKey);
                assign_suit_string(
                    "locked_description_key",
                    suit.LockedDescriptionKey);
                assign_suit_string(
                    "playable",
                    suit.PlayablePackage);
                assign_suit_string(
                    "cutscene",
                    suit.CutscenePackage);
                assign_suit_string(
                    "dcmd",
                    suit.DcmdPackage);

                suit.PlayableClassPath =
                    canonical_blueprint_generated_class_path(
                        suit.PlayablePackage);
                suit.CutsceneClassPath =
                    canonical_blueprint_generated_class_path(
                        suit.CutscenePackage);
                suit.DcmdObjectPath =
                    canonical_object_path(
                        suit.DcmdPackage);

                const auto dcmd_slash =
                    suit.DcmdPackage.find_last_of(
                        STR("/"));
                if (dcmd_slash != StringType::npos)
                {
                    suit.DcmdPackagePath =
                        suit.DcmdPackage.substr(
                            0,
                            dcmd_slash);
                    if (dcmd_slash + 1 <
                        suit.DcmdPackage.size())
                    {
                        suit.DcmdAssetName =
                            suit.DcmdPackage.substr(
                                dcmd_slash + 1);
                    }
                }

                if (suit.SuitId.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-suit_id"));
                }

                if (suit.CharacterScope.empty())
                {
                    // character_scope was added after schema-3 shipped.  Keep
                    // every existing pack compatible by deriving its owner
                    // from the canonical PawnTag instead of rejecting it.
                    suit.CharacterScope =
                        derive_native_manifest_character_scope(
                            suit.PawnTag);
                }

                if (suit.PawnTag.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-pawn_tag"));
                }
                else if (!tag_identity_starts_with(
                             suit.PawnTag,
                             STR("Pawns.Playable.")))
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("pawn_tag-not-playable"));
                }
                if (suit.CharacterScope.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-character_scope"));
                }
                else if (!tag_identity_starts_with(
                             suit.CharacterScope,
                             STR("Pawns.Playable.")))
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("character_scope-not-playable"));
                }
                else if (const auto derived_scope =
                             derive_native_manifest_character_scope(
                                 suit.PawnTag);
                         !derived_scope.empty() &&
                         !tag_identity_equals(
                             suit.CharacterScope,
                             derived_scope))
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("character_scope-does-not-match-pawn_tag"));
                }
                if (suit.ProgressTag.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-progress_tag"));
                }
                if (suit.PlayablePackage.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-playable"));
                }
                if (suit.CutscenePackage.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-cutscene"));
                }
                if (suit.DcmdPackage.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-dcmd"));
                }
                else if (!starts_with(
                             suit.DcmdPackage,
                             STR("/Game/Mods/")))
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("dcmd-not-under-game-mods"));
                }
                if (suit.DcmdAssetName.empty() ||
                    suit.DcmdPackagePath.empty() ||
                    suit.DcmdObjectPath.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("dcmd-path-normalization-failed"));
                }
                if (suit.DisplayNameKey.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-display_name_key"));
                }
                if (suit.DescriptionKey.empty())
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("missing-description_key"));
                }

                suit.Valid =
                    suit.ValidationError.empty();

                const bool duplicate_suit_id =
                    !suit.SuitId.empty() &&
                    std::any_of(
                        manifest.Suits.begin(),
                        manifest.Suits.end(),
                        [&](const NativeSuitModManifestSuit& existing)
                        {
                            return
                                existing.SuitId ==
                                suit.SuitId;
                        });
                const bool duplicate_pawn_tag =
                    !suit.PawnTag.empty() &&
                    std::any_of(
                        manifest.Suits.begin(),
                        manifest.Suits.end(),
                        [&](const NativeSuitModManifestSuit& existing)
                        {
                            return
                                tag_identity_equals(
                                    existing.PawnTag,
                                    suit.PawnTag);
                        });

                if (duplicate_suit_id)
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("duplicate-suit_id-in-mod"));
                }
                if (duplicate_pawn_tag)
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("duplicate-pawn_tag-in-mod"));
                }
                suit.Valid =
                    suit.ValidationError.empty();

                manifest.Suits.push_back(
                    std::move(suit));
            }
        }
        if (manifest.SchemaVersion != 3)
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("unsupported-schema-version"));
        }
        if (manifest.Format !=
            STR("native_suit_mod"))
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("unsupported-format"));
        }
        if (manifest.ModId.empty())
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("missing-mod_id"));
        }
        if (manifest.ContentRoot.empty())
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("missing-content_root"));
        }
        if (manifest.StringTable.empty())
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("missing-string_table"));
        }
        if (!has_suits_array)
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("missing-content-arrays"));
        }
        if (manifest.Suits.empty())
        {
            append_native_manifest_validation_error(
                manifest.ValidationError,
                STR("no-content-entries"));
        }

        std::stable_sort(
            manifest.Suits.begin(),
            manifest.Suits.end(),
            [](const NativeSuitModManifestSuit& left,
               const NativeSuitModManifestSuit& right)
            {
                if (left.MenuOrder != right.MenuOrder)
                {
                    return
                        left.MenuOrder <
                        right.MenuOrder;
                }
                return
                    left.SuitId <
                    right.SuitId;
            });

        manifest.Valid =
            manifest.ValidationError.empty();
        return manifest;
    }

    const std::vector<NativeSuitModManifest>&
    load_native_suit_mod_manifests_log_only()
    {
        if (NativeSuitModManifestsLoaded)
        {
            return
                LoadedNativeSuitModManifests;
        }

        LoadedNativeSuitModManifests.clear();

        const auto files =
            find_native_suit_mod_json_files();

        size_t total_suits = 0;
        size_t enabled_suits = 0;
        size_t valid_enabled_suits = 0;

        for (const auto& file : files)
        {
            auto manifest =
                parse_native_suit_mod_manifest(
                    file);

            const bool duplicate_mod_id =
                !manifest.ModId.empty() &&
                std::any_of(
                    LoadedNativeSuitModManifests.begin(),
                    LoadedNativeSuitModManifests.end(),
                    [&](const NativeSuitModManifest& existing)
                    {
                        return
                            existing.ModId ==
                            manifest.ModId;
                    });

            if (duplicate_mod_id)
            {
                append_native_manifest_validation_error(
                    manifest.ValidationError,
                    STR("duplicate-mod_id"));
                manifest.Valid = false;
            }

            for (auto& suit : manifest.Suits)
            {
                const bool duplicate_global_pawn_tag =
                    !suit.PawnTag.empty() &&
                    std::any_of(
                        LoadedNativeSuitModManifests.begin(),
                        LoadedNativeSuitModManifests.end(),
                        [&](const NativeSuitModManifest& existing_manifest)
                        {
                            return
                                std::any_of(
                                    existing_manifest.Suits.begin(),
                                    existing_manifest.Suits.end(),
                                    [&](const NativeSuitModManifestSuit& existing_suit)
                                    {
                                        return
                                            tag_identity_equals(
                                                existing_suit.PawnTag,
                                                suit.PawnTag);
                                    });
                        });

                if (duplicate_global_pawn_tag)
                {
                    append_native_manifest_validation_error(
                        suit.ValidationError,
                        STR("duplicate-pawn_tag-across-mods"));
                    suit.Valid = false;
                }
            }

            suit_vlog(
                STR("{} SuitRuntime NATIVE_MOD_MANIFEST_CANDIDATE path={} loaded={} valid={} reason={} schema={} format={} mod_id={} display_name={} package_base_name={} content_root={} string_table={} build_id={} suits={}\n"),
                ModTag,
                file.wstring(),
                manifest.LoadedFromJson
                    ? STR("yes")
                    : STR("no"),
                manifest.Valid
                    ? STR("yes")
                    : STR("no"),
                manifest.ValidationError.empty()
                    ? STR("<none>")
                    : manifest.ValidationError,
                manifest.SchemaVersion,
                manifest.Format.empty()
                    ? STR("<none>")
                    : manifest.Format,
                manifest.ModId.empty()
                    ? STR("<none>")
                    : manifest.ModId,
                manifest.DisplayName.empty()
                    ? STR("<none>")
                    : manifest.DisplayName,
                manifest.PackageBaseName.empty()
                    ? STR("<none>")
                    : manifest.PackageBaseName,
                manifest.ContentRoot.empty()
                    ? STR("<none>")
                    : manifest.ContentRoot,
                manifest.StringTable.empty()
                    ? STR("<none>")
                    : manifest.StringTable,
                manifest.BuildId.empty()
                    ? STR("<none>")
                    : manifest.BuildId,
                manifest.Suits.size());

            for (size_t suit_index = 0;
                 suit_index < manifest.Suits.size();
                 ++suit_index)
            {
                const auto& suit =
                    manifest.Suits[suit_index];

                ++total_suits;
                if (suit.Enabled)
                {
                    ++enabled_suits;
                }
                if (manifest.Valid &&
                    suit.Enabled &&
                    suit.Valid)
                {
                    ++valid_enabled_suits;
                }

                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MOD_MANIFEST_SUIT mod_id={} index={} suit_id={} enabled={} valid={} reason={} menu_order={} pawn_tag={} progress_tag={} dcmd_package={} dcmd_object={} dcmd_package_path={} dcmd_asset_name={} playable_package={} playable_class={} cutscene_package={} cutscene_class={} display_name_key={} description_key={} locked_description_key={} policy=log-only-not-registered\n"),
                    ModTag,
                    manifest.ModId.empty()
                        ? STR("<none>")
                        : manifest.ModId,
                    suit_index,
                    suit.SuitId.empty()
                        ? STR("<none>")
                        : suit.SuitId,
                    suit.Enabled
                        ? STR("yes")
                        : STR("no"),
                    suit.Valid
                        ? STR("yes")
                        : STR("no"),
                    suit.ValidationError.empty()
                        ? STR("<none>")
                        : suit.ValidationError,
                    suit.MenuOrder,
                    suit.PawnTag.empty()
                        ? STR("<none>")
                        : suit.PawnTag,
                    suit.ProgressTag.empty()
                        ? STR("<none>")
                        : suit.ProgressTag,
                    suit.DcmdPackage.empty()
                        ? STR("<none>")
                        : suit.DcmdPackage,
                    suit.DcmdObjectPath.empty()
                        ? STR("<none>")
                        : suit.DcmdObjectPath,
                    suit.DcmdPackagePath.empty()
                        ? STR("<none>")
                        : suit.DcmdPackagePath,
                    suit.DcmdAssetName.empty()
                        ? STR("<none>")
                        : suit.DcmdAssetName,
                    suit.PlayablePackage.empty()
                        ? STR("<none>")
                        : suit.PlayablePackage,
                    suit.PlayableClassPath.empty()
                        ? STR("<none>")
                        : suit.PlayableClassPath,
                    suit.CutscenePackage.empty()
                        ? STR("<none>")
                        : suit.CutscenePackage,
                    suit.CutsceneClassPath.empty()
                        ? STR("<none>")
                        : suit.CutsceneClassPath,
                    suit.DisplayNameKey.empty()
                        ? STR("<none>")
                        : suit.DisplayNameKey,
                    suit.DescriptionKey.empty()
                        ? STR("<none>")
                        : suit.DescriptionKey,
                    suit.LockedDescriptionKey.empty()
                        ? STR("<none>")
                        : suit.LockedDescriptionKey);
            }

            if (!manifest.Valid)
            {
                suit_log(
                    STR("{} ERROR manifest rejected: file={} mod_id={} reason={}\n"),
                    ModTag,
                    file.wstring(),
                    manifest.ModId.empty() ? STR("<missing>") : manifest.ModId,
                    manifest.ValidationError.empty()
                        ? STR("unknown-validation-error")
                        : manifest.ValidationError);
            }

            for (const auto& rejected_suit : manifest.Suits)
            {
                if (rejected_suit.Enabled &&
                    (!manifest.Valid || !rejected_suit.Valid))
                {
                    suit_log(
                        STR("{} ERROR suit rejected: mod_id={} suit_id={} pawn_tag={} reason={}\n"),
                        ModTag,
                        manifest.ModId.empty() ? STR("<missing>") : manifest.ModId,
                        rejected_suit.SuitId.empty() ? STR("<missing>") : rejected_suit.SuitId,
                        rejected_suit.PawnTag.empty() ? STR("<missing>") : rejected_suit.PawnTag,
                        rejected_suit.ValidationError.empty()
                            ? manifest.ValidationError
                            : rejected_suit.ValidationError);
                }
            }

            LoadedNativeSuitModManifests.push_back(
                std::move(manifest));
        }

        NativeSuitModManifestsLoaded = true;
        NativeSchema3ManifestModeActive =
            valid_enabled_suits > 0;

        suit_vlog(
            STR("{} SuitRuntime NATIVE_MOD_MANIFEST_SUMMARY files={} manifests={} valid_manifests={} suits={} enabled_suits={} valid_enabled_suits={} native_mode={} registration_source=schema3-all-enabled-valid-a3 next=verify-every-enabled-suit-registers-and-appears-natively\n"),
            ModTag,
            files.size(),
            LoadedNativeSuitModManifests.size(),
            std::count_if(
                LoadedNativeSuitModManifests.begin(),
                LoadedNativeSuitModManifests.end(),
                [](const NativeSuitModManifest& manifest)
                {
                    return
                        manifest.Valid;
                }),
            total_suits,
            enabled_suits,
            valid_enabled_suits,
            NativeSchema3ManifestModeActive
                ? STR("schema3-native-registration-only")
                : STR("legacy-json-fallback"));

        return
            LoadedNativeSuitModManifests;
    }

    const NativeSuitModManifestSuit*
    find_enabled_native_manifest_suit_by_pawn_tag(
        StringViewType pawn_tag,
        const NativeSuitModManifest*& out_manifest)
    {
        out_manifest = nullptr;

        for (const auto& manifest :
             LoadedNativeSuitModManifests)
        {
            if (!manifest.Valid)
            {
                continue;
            }

            for (const auto& suit :
                 manifest.Suits)
            {
                if (suit.Enabled &&
                    suit.Valid &&
                    tag_identity_equals(
                        suit.PawnTag,
                        pawn_tag))
                {
                    out_manifest = &manifest;
                    return &suit;
                }
            }
        }

        return nullptr;
    }

    std::filesystem::path saved_native_manifest_selection_file_path()
    {
        return (lotdk_expanded_root() /
                L"Saved" /
                L"selected_native_suit.json").lexically_normal();
    }

    std::vector<std::filesystem::path>
    legacy_saved_native_manifest_selection_file_paths()
    {
        const auto ue4ss_root =
            lotdk_expanded_root().parent_path();
        return {
            (ue4ss_root /
             L"Mods" /
             L"NewSuitSlotNative" /
             L"Saved" /
             L"selected_native_suit.json").lexically_normal(),
            (ue4ss_root /
             L"Mods" /
             L"LOTDKExpanded" /
             L"Saved" /
             L"selected_native_suit.json").lexically_normal()
        };
    }

    NativeManifestSavedSelection*
    find_saved_native_manifest_selection_for_scope(
        StringViewType character_scope)
    {
        const auto it =
            std::find_if(
                SavedNativeManifestSelections.begin(),
                SavedNativeManifestSelections.end(),
                [&](const NativeManifestSavedSelection& selection)
                {
                    return tag_identity_equals(
                        selection.CharacterScope,
                        character_scope);
                });
        return it == SavedNativeManifestSelections.end()
            ? nullptr
            : &(*it);
    }

    const NativeManifestSavedSelection*
    find_saved_native_manifest_selection_for_scope(
        StringViewType character_scope,
        const std::vector<NativeManifestSavedSelection>& selections)
    {
        const auto it =
            std::find_if(
                selections.begin(),
                selections.end(),
                [&](const NativeManifestSavedSelection& selection)
                {
                    return tag_identity_equals(
                        selection.CharacterScope,
                        character_scope);
                });
        return it == selections.end()
            ? nullptr
            : &(*it);
    }

    // Commit a proposed per-character selection document atomically. Callers do
    // not publish the proposed state in memory until this succeeds, so a sharing
    // violation cannot turn a failed write into a false "already current" result
    // on the following BP_OnClicked event.
    bool commit_saved_native_manifest_selections(
        const std::vector<NativeManifestSavedSelection>& selections,
        StringViewType last_active_character_scope,
        StringViewType operation)
    {
        const auto path = saved_native_manifest_selection_file_path();
        std::error_code error{};

        // Keep a canonical schema-2 document even when the final selection is
        // cleared. An empty file is a durable tombstone: without it, a migrated
        // legacy sidecar would become visible again on the next launch as soon
        // as the canonical file was deleted.

        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            suit_log(
                STR("{} ERROR selection: could not create save folder path={} reason={}\n"),
                ModTag,
                path.parent_path().wstring(),
                widen_utf8(error.message()));
            return false;
        }

        const auto temporary_path =
            std::filesystem::path{path.wstring() + L".tmp"};
        std::ofstream file(
            temporary_path,
            std::ios::binary | std::ios::trunc);
        if (!file)
        {
            suit_log(
                STR("{} ERROR selection: could not write temporary file {}\n"),
                ModTag,
                temporary_path.wstring());
            return false;
        }

        file << "{\n";
        file << "  \"schema_version\": 2,\n";
        file << "  \"selection_type\": \"native_manifest_character_selections\",\n";
        file << "  \"last_active_character_scope\": \""
             << json_escape_string(
                    narrow_utf8(
                        selections.empty()
                            ? StringViewType{}
                            : last_active_character_scope))
             << "\",\n";
        file << "  \"selections\": [\n";
        for (size_t index = 0;
             index < selections.size();
             ++index)
        {
            const auto& selection =
                selections[index];
            file << "    {\n";
            file << "      \"character_scope\": \""
                 << json_escape_string(
                        narrow_utf8(selection.CharacterScope))
                 << "\",\n";
            file << "      \"mod_id\": \""
                 << json_escape_string(narrow_utf8(selection.ModId))
                 << "\",\n";
            file << "      \"suit_id\": \""
                 << json_escape_string(narrow_utf8(selection.SuitId))
                 << "\",\n";
            file << "      \"pawn_tag\": \""
                 << json_escape_string(narrow_utf8(selection.PawnTag))
                 << "\"\n";
            file << "    }"
                 << (index + 1 < selections.size()
                         ? ",\n"
                         : "\n");
        }
        file << "  ]\n";
        file << "}\n";
        file.flush();
        file.close();
        const bool stream_ok = file.good();
        std::error_code temporary_verify_error{};
        const bool temporary_exists =
            std::filesystem::is_regular_file(
                temporary_path,
                temporary_verify_error);

        if (!stream_ok ||
            !temporary_exists)
        {
            std::error_code cleanup_error{};
            std::filesystem::remove(temporary_path, cleanup_error);
            suit_log(
                STR("{} ERROR selection: temporary save write failed path={} reason={}\n"),
                ModTag,
                temporary_path.wstring(),
                !stream_ok
                    ? STR("stream-close-or-flush-failed")
                    : temporary_verify_error
                          ? widen_utf8(temporary_verify_error.message())
                          : STR("temporary-file-missing"));
            return false;
        }

        if (!MoveFileExW(
                temporary_path.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD win32_error = GetLastError();
            std::error_code cleanup_error{};
            std::filesystem::remove(temporary_path, cleanup_error);
            suit_log(
                STR("{} ERROR selection: could not commit saved suit file path={} win32_error={} operation={}\n"),
                ModTag,
                path.wstring(),
                win32_error,
                operation.empty() ? STR("<unknown>") : operation);
            return false;
        }

        std::error_code verify_error{};
        if (!std::filesystem::is_regular_file(path, verify_error))
        {
            suit_log(
                STR("{} ERROR selection: committed saved suit file could not be verified path={} reason={} operation={}\n"),
                ModTag,
                path.wstring(),
                verify_error
                    ? widen_utf8(verify_error.message())
                    : STR("file-missing-after-commit"),
                operation.empty() ? STR("<unknown>") : operation);
            return false;
        }

        SavedNativeManifestCanonicalDocumentValidated = true;
        return true;
    }

    bool write_saved_native_manifest_selection(
        const NativeSuitModManifest& manifest,
        const NativeSuitModManifestSuit& suit,
        StringViewType trigger)
    {
        SelectionTiming timing{L"save-selection"};
        const auto character_scope =
            native_manifest_character_scope_for_suit(suit);
        if (manifest.ModId.empty() ||
            suit.SuitId.empty() ||
            suit.PawnTag.empty() ||
            character_scope.empty())
        {
            suit_log(
                STR("{} ERROR selection: manifest identity or character scope is incomplete; selection was not saved\n"),
                ModTag);
            return false;
        }

        std::error_code sidecar_error{};
        const bool sidecar_exists =
            std::filesystem::is_regular_file(
                saved_native_manifest_selection_file_path(),
                sidecar_error);
        auto proposed_selections =
            SavedNativeManifestSelections;
        const auto proposed_it =
            std::find_if(
                proposed_selections.begin(),
                proposed_selections.end(),
                [&](const NativeManifestSavedSelection& selection)
                {
                    return tag_identity_equals(
                        selection.CharacterScope,
                        character_scope);
                });
        const bool unchanged =
            proposed_it != proposed_selections.end() &&
            proposed_it->ManifestResolved &&
            tag_identity_equals(proposed_it->PawnTag, suit.PawnTag) &&
            proposed_it->ModId == manifest.ModId &&
            proposed_it->SuitId == suit.SuitId &&
            tag_identity_equals(
                SavedNativeManifestLastActiveCharacterScope,
                character_scope) &&
            !SavedNativeManifestSelectionsNeedMigration &&
            SavedNativeManifestCanonicalDocumentValidated &&
            !sidecar_error &&
            sidecar_exists;

        NativeManifestSavedSelection selection{};
        selection.CharacterScope = character_scope;
        selection.ModId = manifest.ModId;
        selection.SuitId = suit.SuitId;
        selection.PawnTag = suit.PawnTag;
        selection.ManifestResolved = true;
        if (proposed_it != proposed_selections.end())
        {
            *proposed_it = selection;
        }
        else
        {
            proposed_selections.push_back(
                std::move(selection));
        }

        if (!unchanged &&
            !commit_saved_native_manifest_selections(
                proposed_selections,
                character_scope,
                trigger))
        {
            suit_log(
                STR("{} ERROR selection: custom selection commit failed; in-memory state was not changed and the next activation may retry scope={} pawn_tag={} trigger={}\n"),
                ModTag,
                character_scope,
                suit.PawnTag,
                trigger.empty() ? STR("<unknown>") : trigger);
            return false;
        }

        // Publish only a document known to exist on disk (or an unchanged,
        // independently verified existing document).
        SavedNativeManifestSelections =
            std::move(proposed_selections);
        SavedNativeManifestSelectionLoaded = true;
        SavedNativeManifestSelectionsNeedMigration = false;
        SavedNativeManifestActiveCharacterScope = character_scope;
        SavedNativeManifestLastActiveCharacterScope = character_scope;
        SavedNativeManifestPawnTag = suit.PawnTag;
        SavedNativeManifestModId = manifest.ModId;
        SavedNativeManifestSuitId = suit.SuitId;

        // Keep both generated classes resident while the native menu hands the
        // selected identity to gameplay or the frontend.
        NativeManifestStartupClassPreloadArmed = true;
        NativeManifestStartupClassPreloadAttempted = false;
        NativeManifestStartupClassPreloadComplete = false;
        NativeManifestStartupClassPreloadStartedMs = 0;
        NativeManifestStartupClassPreloadNextPollMs = 0;
        NativeManifestStartupClassPreloadPollCount = 0;
        NativeManifestStartupClassPreloadPawnTag = suit.PawnTag;

        // A live native button selection already performs its own handover. The
        // automatic gameplay fallback is only armed after a cold-start sidecar read.
        NativeManifestStartupRestoreArmed = false;
        NativeManifestStartupRestoreDispatched = false;
        NativeManifestStartupRestoreComplete = true;
        NativeManifestStartupRestoreStartedMs = 0;
        NativeManifestStartupRestoreNextPollMs = 0;
        NativeManifestStartupRestoreAuditDueMs = 0;
        NativeManifestStartupRestorePollCount = 0;
        NativeManifestStartupRestorePawnTag = suit.PawnTag;

        if (unchanged)
        {
            suit_vlog(
                STR("{} Selection already current: scope={} suit_id={} pawn_tag={} trigger={}\n"),
                ModTag,
                character_scope,
                suit.SuitId,
                suit.PawnTag,
                trigger.empty() ? STR("<unknown>") : trigger);
            return true;
        }

        suit_log(
            STR("{} Selected custom suit: scope={} suit_id={} pawn_tag={}\n"),
            ModTag,
            character_scope,
            suit.SuitId,
            suit.PawnTag);
        return true;
    }

    bool clear_saved_native_manifest_selection(
        StringViewType character_scope,
        StringViewType trigger)
    {
        if (character_scope.empty())
        {
            suit_vlog(
                STR("{} Ignored base-game suit activation with no character scope; trigger={}\n"),
                ModTag,
                trigger.empty() ? STR("<unknown>") : trigger);
            return true;
        }

        auto proposed_selections =
            SavedNativeManifestSelections;
        const auto proposed_it =
            std::find_if(
                proposed_selections.begin(),
                proposed_selections.end(),
                [&](const NativeManifestSavedSelection& selection)
                {
                    return tag_identity_equals(
                        selection.CharacterScope,
                        character_scope);
                });
        const bool had_custom_selection =
            proposed_it != proposed_selections.end();
        if (had_custom_selection)
        {
            proposed_selections.erase(proposed_it);
        }

        const bool clearing_active_scope =
            tag_identity_equals(
                SavedNativeManifestActiveCharacterScope,
                character_scope);
        auto proposed_last_active_character_scope =
            SavedNativeManifestLastActiveCharacterScope;
        bool last_active_scope_changed = false;
        if (tag_identity_equals(
                proposed_last_active_character_scope,
                character_scope))
        {
            // The player explicitly chose a native suit for this character.
            // Do not let a different character's remembered custom suit become
            // a surprise startup restore target.
            proposed_last_active_character_scope.clear();
            last_active_scope_changed = true;
        }

        std::error_code sidecar_error{};
        const bool sidecar_exists =
            std::filesystem::is_regular_file(
                saved_native_manifest_selection_file_path(),
                sidecar_error);
        if (sidecar_error)
        {
            suit_log(
                STR("{} ERROR selection: base-game selection could not inspect persisted state; in-memory state was not changed and the next activation may retry scope={} reason={}\n"),
                ModTag,
                character_scope,
                widen_utf8(sidecar_error.message()));
            return false;
        }

        const bool document_needs_commit =
            had_custom_selection ||
            last_active_scope_changed ||
            SavedNativeManifestSelectionsNeedMigration ||
            !sidecar_exists ||
            !SavedNativeManifestCanonicalDocumentValidated;
        if (document_needs_commit &&
            !commit_saved_native_manifest_selections(
                proposed_selections,
                proposed_last_active_character_scope,
                trigger))
        {
            suit_log(
                STR("{} ERROR selection: base-game selection commit failed; in-memory state was not changed and the next activation may retry scope={} trigger={}\n"),
                ModTag,
                character_scope,
                trigger.empty() ? STR("<unknown>") : trigger);
            return false;
        }

        // Publish only after the matching sidecar mutation has succeeded. This
        // keeps a later Clicked notification retryable after a sharing violation
        // instead of silently losing the old selection in memory.
        SavedNativeManifestSelections =
            std::move(proposed_selections);
        SavedNativeManifestLastActiveCharacterScope =
            std::move(proposed_last_active_character_scope);
        SavedNativeManifestSelectionLoaded = true;
        if (document_needs_commit)
        {
            SavedNativeManifestSelectionsNeedMigration = false;
        }

        if (clearing_active_scope)
        {
            SavedNativeManifestPawnTag.clear();
            SavedNativeManifestModId.clear();
            SavedNativeManifestSuitId.clear();
            SavedNativeManifestActiveCharacterScope.clear();

            NativeManifestStartupClassPreloadArmed = false;
            NativeManifestStartupClassPreloadAttempted = false;
            NativeManifestStartupClassPreloadComplete = true;
            NativeManifestStartupClassPreloadStartedMs = 0;
            NativeManifestStartupClassPreloadNextPollMs = 0;
            NativeManifestStartupClassPreloadPollCount = 0;
            NativeManifestStartupClassPreloadPawnTag.clear();

            NativeManifestStartupRestoreArmed = false;
            NativeManifestStartupRestoreDispatched = false;
            NativeManifestStartupRestoreComplete = true;
            NativeManifestStartupRestoreStartedMs = 0;
            NativeManifestStartupRestoreNextPollMs = 0;
            NativeManifestStartupRestoreAuditDueMs = 0;
            NativeManifestStartupRestorePollCount = 0;
            NativeManifestStartupRestorePawnTag.clear();
            NativeManifestStartupRestoreGameplayObservedMs = 0;
            NativeManifestStartupRestoreReadySinceMs = 0;
            NativeManifestStartupRestoreReadyObservations = 0;
            NativeManifestStartupRestoreReadySignature.clear();
        }

        if (!had_custom_selection)
        {
            suit_vlog(
                STR("{} Base-game suit activation observed with no custom selection for scope={} trigger={}\n"),
                ModTag,
                character_scope,
                trigger.empty() ? STR("<unknown>") : trigger);
            return true;
        }

        suit_log(
            STR("{} Selected a base-game suit; custom startup restore cleared for scope={}\n"),
            ModTag,
            character_scope);
        return true;
    }

    bool read_saved_native_manifest_selection(
        StringViewType trigger)
    {
        SavedNativeManifestSelectionLoaded = true;
        SavedNativeManifestPawnTag.clear();
        SavedNativeManifestModId.clear();
        SavedNativeManifestSuitId.clear();
        SavedNativeManifestActiveCharacterScope.clear();
        SavedNativeManifestLastActiveCharacterScope.clear();
        SavedNativeManifestSelections.clear();
        SavedNativeManifestSelectionsNeedMigration = false;
        SavedNativeManifestCanonicalDocumentValidated = false;

        NativeManifestStartupClassPreloadArmed = false;
        NativeManifestStartupClassPreloadAttempted = false;
        NativeManifestStartupClassPreloadComplete = false;
        NativeManifestStartupClassPreloadStartedMs = 0;
        NativeManifestStartupClassPreloadNextPollMs = 0;
        NativeManifestStartupClassPreloadPollCount = 0;
        NativeManifestStartupClassPreloadPawnTag.clear();

        NativeManifestStartupRestoreArmed = false;
        NativeManifestStartupRestoreDispatched = false;
        NativeManifestStartupRestoreComplete = false;
        NativeManifestStartupRestoreStartedMs = 0;
        NativeManifestStartupRestoreNextPollMs = 0;
        NativeManifestStartupRestoreAuditDueMs = 0;
        NativeManifestStartupRestorePollCount = 0;
        NativeManifestStartupRestorePawnTag.clear();
        NativeManifestStartupRestoreGameplayObservedMs = 0;
        NativeManifestStartupRestoreReadySinceMs = 0;
        NativeManifestStartupRestoreReadyObservations = 0;
        NativeManifestStartupRestoreReadySignature.clear();

        const auto current_path =
            saved_native_manifest_selection_file_path();
        auto path = current_path;
        bool loaded_from_legacy_path = false;
        std::error_code exists_error{};
        bool file_exists =
            std::filesystem::is_regular_file(path, exists_error);
        if (!file_exists && !exists_error)
        {
            for (const auto& legacy_path :
                 legacy_saved_native_manifest_selection_file_paths())
            {
                std::error_code legacy_error{};
                if (std::filesystem::is_regular_file(
                        legacy_path,
                        legacy_error))
                {
                    path = legacy_path;
                    file_exists = true;
                    loaded_from_legacy_path = true;
                    break;
                }
                if (legacy_error)
                {
                    suit_vlog(
                        STR("{} Selection migration: legacy path could not be inspected path={} reason={}\n"),
                        ModTag,
                        legacy_path.wstring(),
                        widen_utf8(legacy_error.message()));
                }
            }
        }
        if (!file_exists)
        {
            if (exists_error)
            {
                suit_log(
                    STR("{} ERROR selection: could not inspect saved suit path={} reason={}\n"),
                    ModTag,
                    path.wstring(),
                    widen_utf8(exists_error.message()));
            }
            else
            {
                suit_vlog(
                    STR("{} No saved custom suit; trigger={}\n"),
                    ModTag,
                    trigger.empty() ? STR("<unknown>") : trigger);
            }
            return false;
        }

        if (loaded_from_legacy_path)
        {
            suit_log(
                STR("{} Selection migration: loading legacy save path={} destination={}\n"),
                ModTag,
                path.wstring(),
                current_path.wstring());
            SavedNativeManifestSelectionsNeedMigration = true;
        }

        const auto contents = read_text_file(path);
        if (contents.empty())
        {
            suit_log(
                STR("{} ERROR selection: saved suit file is empty or unreadable path={}\n"),
                ModTag,
                path.wstring());
            return false;
        }

        int32_t schema_version = 0;
        json_find_int_value(contents, "schema_version", schema_version);

        const auto add_selection =
            [&](StringViewType requested_scope,
                StringViewType requested_mod_id,
                StringViewType requested_suit_id,
                StringViewType requested_pawn_tag,
                StringType& out_reason) -> bool
            {
                out_reason.clear();
                if (requested_pawn_tag.empty())
                {
                    out_reason = STR("missing-pawn_tag");
                    return false;
                }

                // PawnTag is the stable runtime identity. Mod/suit ids are
                // authoring metadata and can legitimately change when a saved
                // project is relinked or moved between Batcomputer mods.
                const auto pawn_derived_scope =
                    derive_native_manifest_character_scope(
                        requested_pawn_tag);
                if (pawn_derived_scope.empty())
                {
                    out_reason = STR("pawn-tag-has-no-playable-character-scope");
                    return false;
                }

                const NativeSuitModManifest* manifest = nullptr;
                const auto* manifest_suit =
                    find_enabled_native_manifest_suit_by_pawn_tag(
                        requested_pawn_tag,
                        manifest);
                if (!manifest || !manifest_suit)
                {
                    if (find_saved_native_manifest_selection_for_scope(
                            pawn_derived_scope,
                            SavedNativeManifestSelections))
                    {
                        out_reason = STR("duplicate-character_scope");
                        return false;
                    }

                    NativeManifestSavedSelection dormant_selection{};
                    dormant_selection.CharacterScope = pawn_derived_scope;
                    dormant_selection.ModId = requested_mod_id;
                    dormant_selection.SuitId = requested_suit_id;
                    dormant_selection.PawnTag = requested_pawn_tag;
                    dormant_selection.ManifestResolved = false;
                    SavedNativeManifestSelections.push_back(
                        std::move(dormant_selection));

                    if (!requested_scope.empty() &&
                        requested_scope != pawn_derived_scope)
                    {
                        SavedNativeManifestSelectionsNeedMigration = true;
                    }
                    suit_vlog(
                        STR("{} Selection preserved dormant: scope={} pawn_tag={} reason=manifest-currently-unavailable\n"),
                        ModTag,
                        pawn_derived_scope,
                        requested_pawn_tag);
                    return true;
                }

                const auto expected_scope =
                    native_manifest_character_scope_for_suit(
                        *manifest_suit);
                if (expected_scope.empty())
                {
                    out_reason = STR("missing-character_scope");
                    return false;
                }
                if (!tag_identity_equals(
                        pawn_derived_scope,
                        expected_scope))
                {
                    out_reason =
                        STR("manifest-character-scope-does-not-match-pawn-tag");
                    return false;
                }
                NativeManifestSavedSelection selection{};
                selection.CharacterScope = expected_scope;
                selection.ModId = manifest->ModId;
                selection.SuitId = manifest_suit->SuitId;
                selection.PawnTag = manifest_suit->PawnTag;
                selection.ManifestResolved = true;
                auto* existing_selection =
                    find_saved_native_manifest_selection_for_scope(
                        expected_scope);
                if (existing_selection)
                {
                    if (existing_selection->ManifestResolved)
                    {
                        out_reason = STR("duplicate-character_scope");
                        return false;
                    }

                    // Prefer the row whose PawnTag resolves now over an earlier
                    // dormant duplicate, then rewrite the document canonically.
                    *existing_selection = selection;
                    SavedNativeManifestSelectionsNeedMigration = true;
                }
                else
                {
                    SavedNativeManifestSelections.push_back(
                        std::move(selection));
                }

                if (requested_scope != expected_scope ||
                    requested_mod_id != manifest->ModId ||
                    requested_suit_id != manifest_suit->SuitId ||
                    requested_pawn_tag != manifest_suit->PawnTag)
                {
                    SavedNativeManifestSelectionsNeedMigration = true;
                    suit_vlog(
                        STR("{} Selection canonicalized from pawn tag: pawn_tag={} canonical_scope={} canonical_mod_id={} canonical_suit_id={}\n"),
                        ModTag,
                        manifest_suit->PawnTag,
                        expected_scope,
                        manifest->ModId,
                        manifest_suit->SuitId);
                }
                return true;
            };

        if (schema_version == 1)
        {
            std::string pawn_tag{};
            std::string mod_id{};
            std::string suit_id{};
            json_find_string_value(contents, "pawn_tag", pawn_tag);
            json_find_string_value(contents, "mod_id", mod_id);
            json_find_string_value(contents, "suit_id", suit_id);

            StringType reason{};
            if (!add_selection(
                    {},
                    widen_utf8(mod_id),
                    widen_utf8(suit_id),
                    widen_utf8(pawn_tag),
                    reason))
            {
                suit_log(
                    STR("{} ERROR selection: saved custom suit was ignored path={} schema=1 pawn_tag={} reason={}\n"),
                    ModTag,
                    path.wstring(),
                    pawn_tag.empty() ? STR("<missing>") : widen_utf8(pawn_tag),
                    reason.empty() ? STR("unknown") : reason);
                return false;
            }

            SavedNativeManifestSelectionsNeedMigration = true;
            SavedNativeManifestLastActiveCharacterScope =
                SavedNativeManifestSelections.front().CharacterScope;
            suit_log(
                STR("{} Loaded legacy schema-1 custom suit; per-character migration is queued now\n"),
                ModTag);
        }
        else if (schema_version == 2)
        {
            std::string last_active_scope{};
            std::string selections_array{};
            json_find_string_value(
                contents,
                "last_active_character_scope",
                last_active_scope);

            if (!json_find_named_array_text(
                    contents,
                    "selections",
                    selections_array))
            {
                suit_log(
                    STR("{} ERROR selection: saved custom suit file has no selections array path={}\n"),
                    ModTag,
                    path.wstring());
                return false;
            }

            for (const auto& selection_text :
                 json_objects_in_array_text(selections_array))
            {
                std::string character_scope{};
                std::string pawn_tag{};
                std::string mod_id{};
                std::string suit_id{};
                json_find_string_value(
                    selection_text,
                    "character_scope",
                    character_scope);
                json_find_string_value(
                    selection_text,
                    "pawn_tag",
                    pawn_tag);
                json_find_string_value(
                    selection_text,
                    "mod_id",
                    mod_id);
                json_find_string_value(
                    selection_text,
                    "suit_id",
                    suit_id);

                StringType reason{};
                if (!add_selection(
                        widen_utf8(character_scope),
                        widen_utf8(mod_id),
                        widen_utf8(suit_id),
                        widen_utf8(pawn_tag),
                        reason))
                {
                    // Keep valid rows, but rewrite the document without a
                    // malformed/duplicate row rather than repeating the same
                    // ambiguity on every startup.
                    SavedNativeManifestSelectionsNeedMigration = true;
                    suit_log(
                        STR("{} ERROR selection: saved character entry ignored scope={} pawn_tag={} reason={}\n"),
                        ModTag,
                        character_scope.empty()
                            ? STR("<missing>")
                            : widen_utf8(character_scope),
                        pawn_tag.empty()
                            ? STR("<missing>")
                            : widen_utf8(pawn_tag),
                        reason.empty() ? STR("unknown") : reason);
                }
            }

            const auto requested_last_active_scope =
                widen_utf8(last_active_scope);
            if (!requested_last_active_scope.empty())
            {
                const auto* last_active_selection =
                    find_saved_native_manifest_selection_for_scope(
                        requested_last_active_scope,
                        SavedNativeManifestSelections);
                if (last_active_selection)
                {
                    SavedNativeManifestLastActiveCharacterScope =
                        last_active_selection->CharacterScope;
                    if (requested_last_active_scope !=
                        last_active_selection->CharacterScope)
                    {
                        SavedNativeManifestSelectionsNeedMigration = true;
                    }
                }
                else
                {
                    // A dangling last-active pointer must not revive an unrelated
                    // character after the rows themselves have been validated.
                    SavedNativeManifestSelectionsNeedMigration = true;
                    suit_vlog(
                        STR("{} Selection migration: dangling last-active scope cleared scope={}\n"),
                        ModTag,
                        requested_last_active_scope);
                }
            }

            if (!loaded_from_legacy_path)
            {
                // The canonical schema and selections array were readable. Any
                // ignored/canonicalized rows keep migration armed, but an empty
                // valid array is still an authoritative tombstone.
                SavedNativeManifestCanonicalDocumentValidated = true;
            }
        }
        else
        {
            suit_log(
                STR("{} ERROR selection: saved custom suit was ignored path={} schema={} reason=unsupported-sidecar-schema\n"),
                ModTag,
                path.wstring(),
                schema_version);
            return false;
        }

        if (loaded_from_legacy_path ||
            SavedNativeManifestSelectionsNeedMigration)
        {
            if (commit_saved_native_manifest_selections(
                    SavedNativeManifestSelections,
                    SavedNativeManifestLastActiveCharacterScope,
                    STR("startup-selection-migration")))
            {
                SavedNativeManifestSelectionsNeedMigration = false;
                suit_log(
                    STR("{} Selection migration complete: source={} destination={} preserved_rows={}\n"),
                    ModTag,
                    path.wstring(),
                    current_path.wstring(),
                    SavedNativeManifestSelections.size());
            }
            else
            {
                // Keep the canonicalized/dormant document in memory and leave
                // migration armed. A later direct suit activation retries it.
                SavedNativeManifestSelectionsNeedMigration = true;
                suit_log(
                    STR("{} ERROR selection: migration commit failed; rows remain in memory and a later direct activation may retry source={} destination={}\n"),
                    ModTag,
                    path.wstring(),
                    current_path.wstring());
            }
        }

        if (SavedNativeManifestSelections.empty())
        {
            suit_vlog(
                STR("{} Saved custom suit document is an empty authoritative selection path={} canonical={}\n"),
                ModTag,
                path.wstring(),
                SavedNativeManifestCanonicalDocumentValidated
                    ? STR("yes")
                    : STR("no"));
            return false;
        }

        const auto resolved_selection_count =
            static_cast<size_t>(std::count_if(
                SavedNativeManifestSelections.begin(),
                SavedNativeManifestSelections.end(),
                [](const NativeManifestSavedSelection& selection)
                {
                    return selection.ManifestResolved;
                }));

        const NativeManifestSavedSelection* active_selection =
            find_saved_native_manifest_selection_for_scope(
                NativeManifestStartupPreloadCharacterScope,
                SavedNativeManifestSelections);
        if (!active_selection ||
            !active_selection->ManifestResolved)
        {
            suit_log(
                STR("{} Saved custom character selections loaded: rows={} resolved={} dormant={} startup_scope={} selection=<none>; startup preload/restore skipped\n"),
                ModTag,
                SavedNativeManifestSelections.size(),
                resolved_selection_count,
                SavedNativeManifestSelections.size() -
                    resolved_selection_count,
                NativeManifestStartupPreloadCharacterScope);
            return true;
        }

        SavedNativeManifestActiveCharacterScope =
            active_selection->CharacterScope;
        SavedNativeManifestPawnTag = active_selection->PawnTag;
        SavedNativeManifestModId = active_selection->ModId;
        SavedNativeManifestSuitId = active_selection->SuitId;

        NativeManifestStartupClassPreloadArmed = true;
        NativeManifestStartupClassPreloadAttempted = false;
        NativeManifestStartupClassPreloadComplete = false;
        NativeManifestStartupClassPreloadStartedMs = 0;
        NativeManifestStartupClassPreloadNextPollMs = 0;
        NativeManifestStartupClassPreloadPollCount = 0;
        NativeManifestStartupClassPreloadPawnTag =
            active_selection->PawnTag;

        NativeManifestStartupRestoreArmed = true;
        NativeManifestStartupRestoreDispatched = false;
        NativeManifestStartupRestoreComplete = false;
        NativeManifestStartupRestoreStartedMs = 0;
        NativeManifestStartupRestoreNextPollMs = 0;
        NativeManifestStartupRestoreAuditDueMs = 0;
        NativeManifestStartupRestorePollCount = 0;
        NativeManifestStartupRestorePawnTag = active_selection->PawnTag;

        suit_log(
            STR("{} Saved custom suit found: scope={} suit_id={} pawn_tag={} saved_character_count={} dormant_character_count={}\n"),
            ModTag,
            SavedNativeManifestActiveCharacterScope,
            SavedNativeManifestSuitId,
            SavedNativeManifestPawnTag,
            SavedNativeManifestSelections.size(),
            SavedNativeManifestSelections.size() -
                resolved_selection_count);
        return true;
    }

    std::filesystem::path mod_settings_file_path()
    {
        return (lotdk_expanded_root() /
                L"settings.json").lexically_normal();
    }

    // rf51: tiny flat-JSON value reader. Finds "key" : <value> and returns the
    // raw token (string contents, or a bare true/false/number). Robust enough
    // for the small flat settings object; it is not a general JSON parser.
    bool settings_text_find_value(
        const std::string& text,
        const std::string& key,
        std::string& out_value)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = text.find(needle);
        if (pos == std::string::npos)
        {
            return false;
        }

        pos += needle.size();

        const auto skip_ws =
            [&]()
            {
                while (pos < text.size() &&
                       (text[pos] == ' ' || text[pos] == '\t' ||
                        text[pos] == '\r' || text[pos] == '\n'))
                {
                    ++pos;
                }
            };

        skip_ws();
        if (pos >= text.size() || text[pos] != ':')
        {
            return false;
        }
        ++pos;
        skip_ws();

        std::string token{};
        if (pos < text.size() && text[pos] == '"')
        {
            ++pos;
            while (pos < text.size() && text[pos] != '"')
            {
                token.push_back(text[pos++]);
            }
        }
        else
        {
            while (pos < text.size() &&
                   text[pos] != ',' && text[pos] != '}' &&
                   text[pos] != '\r' && text[pos] != '\n' &&
                   text[pos] != ' ' && text[pos] != '\t')
            {
                token.push_back(text[pos++]);
            }
        }

        out_value = token;
        return true;
    }

    bool settings_get_bool(
        const std::string& text,
        const std::string& key,
        const bool fallback)
    {
        std::string value{};
        if (!settings_text_find_value(text, key, value))
        {
            return fallback;
        }

        for (auto& character : value)
        {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }

        if (value == "true" || value == "1" || value == "yes")
        {
            return true;
        }
        if (value == "false" || value == "0" || value == "no")
        {
            return false;
        }
        return fallback;
    }

    void write_default_mod_settings_file(
        const std::filesystem::path& path)
{
    std::error_code error{};
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        suit_log(
            STR("{} ERROR settings: could not create folder path={} reason={}\n"),
            ModTag,
            path.parent_path().wstring(),
            widen_utf8(error.message()));
        return;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        suit_log(
            STR("{} ERROR settings: could not create {}\n"),
            ModTag,
            path.wstring());
        return;
    }

    out <<
        "{\n"
        "    \"_comment\": \"LOTDK Expanded settings. Restart the game after editing.\",\n"
        "    \"verbose_logging\": false\n"
        "}\n";
}

    void load_mod_settings()
{
    if (SuitSettingsLoaded)
    {
        return;
    }
    SuitSettingsLoaded = true;
    SuitSettings = SuitModSettings{};

    const auto path = mod_settings_file_path();
    std::error_code error{};
    if (!std::filesystem::exists(path, error))
    {
        write_default_mod_settings_file(path);
        return;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        suit_log(
            STR("{} ERROR settings: could not read {}; verbose logging remains disabled\n"),
            ModTag,
            path.wstring());
        return;
    }

    const std::string text(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    SuitSettings.VerboseLogging =
        settings_get_bool(text, "verbose_logging", false);
    suit_vlog(
        STR("{} Diagnostics enabled from {}\n"),
        ModTag,
        path.wstring());
}


    bool is_live_uobject_pointer(UObject* candidate);

    void tick_preview_idle_baseline_reset();
    void arm_deferred_whip_visual_clone(UObject* pawn, StringViewType reason);


    StringType export_property_value(UObject* object, FProperty* property)
    {
        if (!object || !property)
        {
            return STR("<missing>");
        }

        FString property_text{};
        const auto value_container = property->ContainerPtrToValuePtr<void>(object);
        property->ExportTextItem(property_text, value_container, value_container, object, 0);
        return *property_text;
    }

    StringType export_property_value_from_container(UObject* owner, FProperty* property, void* container)
    {
        if (!property || !container)
        {
            return STR("<missing>");
        }

        FString property_text{};
        const auto value_container = property->ContainerPtrToValuePtr<void>(container);
        property->ExportTextItem(property_text, value_container, value_container, owner, 0);
        return *property_text;
    }

    StringType call_function_return_text(UObject* object, const wchar_t* function_name)
    {
        auto* function = object ? object->GetFunctionByNameInChain(function_name) : nullptr;
        if (!object || !function)
        {
            return STR("<missing function>");
        }

        auto* return_property = function->GetReturnProperty();
        if (!return_property)
        {
            return STR("<no return property>");
        }

        const auto param_size = std::max<int32_t>(function->GetPropertiesSize(), function->GetStructureSize());
        if (param_size <= 0 || param_size > 4096)
        {
            return STR("<bad param size ") + std::to_wstring(param_size) + STR(">");
        }

        std::vector<uint8_t> params(static_cast<size_t>(param_size), 0);
        function->InitializeStruct(params.data());
        object->ProcessEvent(function, params.data());
        const auto result = export_property_value_from_container(object, return_property, params.data());
        function->DestroyStruct(params.data());
        return result;
    }


    UObject* get_object_property_value(UObject* object, const wchar_t* property_name)
    {
        auto* property = object ? object->GetPropertyByNameInChain(property_name) : nullptr;
        auto* object_property = CastField<FObjectPropertyBase>(property);
        if (!object || !object_property)
        {
            return nullptr;
        }

        return object_property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object));
    }


    UObject* get_array_object_element(FArrayProperty* array_property, UObject* object, const int32_t index)
    {
        if (!array_property || !object || !array_property->GetInner())
        {
            return nullptr;
        }

        auto* object_property = CastField<FObjectPropertyBase>(array_property->GetInner());
        if (!object_property)
        {
            return nullptr;
        }

        FScriptArrayHelper helper{array_property, array_property->ContainerPtrToValuePtr<void>(object)};
        if (!helper.IsValidIndex(index))
        {
            return nullptr;
        }

        return object_property->GetObjectPropertyValue(helper.GetRawPtr(index));
    }

    int32_t get_array_property_count(UObject* object, const wchar_t* property_name)
    {
        auto* property = object ? object->GetPropertyByNameInChain(property_name) : nullptr;
        auto* array_property = CastField<FArrayProperty>(property);
        if (!object || !array_property)
        {
            return -1;
        }

        FScriptArrayHelper helper{array_property, array_property->ContainerPtrToValuePtr<void>(object)};
        return helper.Num();
    }

    StringType get_property_text(UObject* object, const wchar_t* property_name)
    {
        auto* property = object ? object->GetPropertyByNameInChain(property_name) : nullptr;
        return property ? export_property_value(object, property) : STR("<missing>");
    }


    UObject* find_loaded_object(const wchar_t* path)
    {
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path);
    }


    bool is_probable_runtime_instance(UObject* object)
    {
        if (!object)
        {
            return false;
        }

        const auto full_name = object->GetFullName();
        if (starts_with(full_name, STR("Class ")) ||
            starts_with(full_name, STR("Package ")) ||
            starts_with(full_name, STR("Function ")) ||
            starts_with(full_name, STR("ScriptStruct ")) ||
            starts_with(full_name, STR("Enum ")) ||
            starts_with(full_name, STR("Blueprint ")) ||
            starts_with(full_name, STR("BlueprintGeneratedClass ")) ||
            starts_with(full_name, STR("WidgetBlueprintGeneratedClass ")))
        {
            return false;
        }

        if (contains(full_name, STR("Default__")) ||
            contains(full_name, STR("/Script/")))
        {
            return false;
        }

        return true;
    }

    bool is_transient_runtime_object(UObject* object)
    {
        if (!object || !is_probable_runtime_instance(object))
        {
            return false;
        }

        return contains(object->GetFullName(), STR("/Engine/Transient"));
    }


    bool is_suit_grid_button(UObject* object)
    {
        if (!object)
        {
            return false;
        }

        return class_name_contains(object, STR("/Game/UI/GameMenu/CharacterSelect/W_CharacterSubMenuButton.W_CharacterSubMenuButton_C"));
    }


    bool suit_menu_contains_button(
        UObject* suit_menu,
        UObject* button)
    {
        if (!suit_menu ||
            !button ||
            !is_suit_grid_button(button))
        {
            return false;
        }

        auto* options_grid =
            get_object_property_value(
                suit_menu,
                STR("OptionsGrid"));
        auto* slots_property =
            options_grid
                ? options_grid->GetPropertyByNameInChain(
                      STR("Slots"))
                : nullptr;
        auto* slots_array =
            CastField<FArrayProperty>(slots_property);
        if (!options_grid || !slots_array)
        {
            return false;
        }

        FScriptArrayHelper slots{
            slots_array,
            slots_array->ContainerPtrToValuePtr<void>(
                options_grid)};
        for (int32_t index = 0;
             index < slots.Num();
             ++index)
        {
            auto* slot =
                get_array_object_element(
                    slots_array,
                    options_grid,
                    index);
            if (get_object_property_value(
                    slot,
                    STR("Content")) == button)
            {
                return true;
            }
        }

        return false;
    }


    UObject* find_live_suit_menu_containing_button(
        UObject* button)
    {
        SelectionTiming timing{L"find-selection-menu"};
        if (!is_suit_grid_button(button))
        {
            return nullptr;
        }

        // Weak identity rejects destroyed/recycled menus. Membership is checked
        // on every hit: never reuse a previous menu's selection evidence.
        static FWeakObjectPtr last_menu{};
        if (auto* cached = last_menu.Get();
            cached && suit_menu_contains_button(cached, button))
        {
            return cached;
        }
        last_menu = FWeakObjectPtr{};

        auto* menu_class = UObjectGlobals::StaticFindObject<UClass*>(
            nullptr, nullptr,
            STR("/Game/UI/GameMenu/CharacterSelect/W_CharacterSuitMenu.W_CharacterSuitMenu_C"));
        UObject* result = nullptr;
        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                // Reject unrelated objects without constructing full names.
                // Keep the original predicate as a fallback if lookup fails.
                if (menu_class && (!object || !object->GetClassPrivate() ||
                    !object->GetClassPrivate()->IsChildOf(menu_class)))
                {
                    return LoopAction::Continue;
                }
                if (!object ||
                    !is_probable_runtime_instance(object) ||
                    !is_transient_runtime_object(object) ||
                    !class_name_contains(
                        object,
                        STR("/Game/UI/GameMenu/CharacterSelect/W_CharacterSuitMenu.W_CharacterSuitMenu_C")))
                {
                    return LoopAction::Continue;
                }

                if (suit_menu_contains_button(object, button))
                {
                    result = object;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        if (result) last_menu = FWeakObjectPtr(result);
        return result;
    }

    UObject* find_first_runtime_object_by_class(StringViewType class_term)
    {
        UObject* result = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* object, ...) -> LoopAction {
            if (!object || !is_probable_runtime_instance(object))
            {
                return LoopAction::Continue;
            }

            auto* object_class = object->GetClassPrivate();
            const auto class_name = object_class ? object_class->GetFullName() : STR("");
            if (contains(class_name, class_term))
            {
                result = object;
                return LoopAction::Break;
            }

            return LoopAction::Continue;
        });

        return result;
    }


    UObject* find_first_runtime_object_derived_from(const wchar_t* class_path)
    {
        auto* target_class =
            class_path
                ? UObjectGlobals::StaticFindObject<UClass*>(
                      nullptr,
                      nullptr,
                      class_path)
                : nullptr;
        if (!target_class)
        {
            return nullptr;
        }

        UObject* result = nullptr;
        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                if (!object || !is_probable_runtime_instance(object))
                {
                    return LoopAction::Continue;
                }

                auto* object_class = object->GetClassPrivate();
                if (object_class && object_class->IsChildOf(target_class))
                {
                    result = object;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        return result;
    }


    bool is_live_uobject_pointer(UObject* candidate);


    UObject* get_object_return_value(void* params, UFunction* function)
    {
        auto* return_property = function ? CastField<FObjectPropertyBase>(function->GetReturnProperty()) : nullptr;
        if (!params || !return_property)
        {
            return nullptr;
        }

        return return_property->GetObjectPropertyValue(return_property->ContainerPtrToValuePtr<void>(params));
    }

    UObject* call_function_object_return_no_params(UObject* object, const wchar_t* function_name)
    {
        auto* function = object ? object->GetFunctionByNameInChain(function_name) : nullptr;
        if (!object || !function || !function->GetReturnProperty())
        {
            return nullptr;
        }

        const auto param_size = std::max<int32_t>(function->GetPropertiesSize(), function->GetStructureSize());
        if (param_size <= 0 || param_size > 1024)
        {
            return nullptr;
        }

        std::vector<uint8_t> params(static_cast<size_t>(param_size), 0);
        function->InitializeStruct(params.data());
        object->ProcessEvent(function, params.data());
        auto* result = get_object_return_value(params.data(), function);
        function->DestroyStruct(params.data());
        return result;
    }

    UObject* get_widget_owning_player(UObject* widget)
    {
        return call_function_object_return_no_params(widget, STR("GetOwningPlayer"));
    }

    UObject* find_first_player_controller()
    {
        return find_first_runtime_object_by_class(STR("PlayerController"));
    }

    bool object_has_function(
        UObject* object,
        const wchar_t* function_name)
    {
        return object &&
            object->GetFunctionByNameInChain(function_name) != nullptr;
    }


    // 0.10.0 build fix v2: the later is_live_uobject_pointer helper is a
    // LOTDKExpanded member after the anonymous namespace is closed. These
    // notification helpers live in the anonymous namespace, so they need their
    // own small implementation, not just a forward declaration.
    bool is_live_uobject_pointer(UObject* candidate)
    {
        if (!candidate)
        {
            return false;
        }

        bool found = false;

        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                if (object == candidate)
                {
                    found = true;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        return found;
    }

    // rf16a compile fix: these helpers are defined later in the anonymous
    // namespace, but rf16's F6/custom-button paths call them earlier. Keep
    // only forward declarations here; behavior is unchanged.
    bool refresh_all_custom_button_labels(StringViewType reason);
    bool apply_custom_menu_text_widgets(const int64_t suit_index);
    bool apply_custom_suit_submenu_text_widgets(const int64_t suit_index);

    int32_t count_suit_grid_buttons(UObject* panel)
    {
        auto* property = panel ? panel->GetPropertyByNameInChain(STR("Slots")) : nullptr;
        auto* array_property = CastField<FArrayProperty>(property);
        if (!panel || !array_property)
        {
            return 0;
        }

        FScriptArrayHelper helper{array_property, array_property->ContainerPtrToValuePtr<void>(panel)};
        int32_t button_count = 0;
        for (int32_t i = 0; i < helper.Num(); ++i)
        {
            auto* slot = get_array_object_element(array_property, panel, i);
            auto* content = get_object_property_value(slot, STR("Content"));
            if (is_suit_grid_button(content))
            {
                ++button_count;
            }
        }

        return button_count;
    }


    bool call_function_bool_return_no_params(UObject* object, const wchar_t* function_name, bool& out_value);


    bool widget_has_direct_menu_signal(UObject* widget)
    {
        if (!widget ||
            !is_live_uobject_pointer(widget))
        {
            return false;
        }

        bool value = false;

        if (call_function_bool_return_no_params(widget, STR("IsHovered"), value) &&
            value)
        {
            return true;
        }

        value = false;

        if (call_function_bool_return_no_params(widget, STR("HasKeyboardFocus"), value) &&
            value)
        {
            return true;
        }

        value = false;

        if (call_function_bool_return_no_params(widget, STR("HasAnyUserFocus"), value) &&
            value)
        {
            return true;
        }

        value = false;

        if (call_function_bool_return_no_params(widget, STR("HasFocusedDescendants"), value) &&
            value)
        {
            return true;
        }

        return false;
    }


    bool suit_grid_has_direct_menu_signal(UObject* suit_menu, UObject* panel)
    {
        if (widget_has_direct_menu_signal(suit_menu) ||
            widget_has_direct_menu_signal(panel))
        {
            return true;
        }

        auto* property = panel ? panel->GetPropertyByNameInChain(STR("Slots")) : nullptr;
        auto* array_property = CastField<FArrayProperty>(property);
        if (!panel || !array_property)
        {
            return false;
        }

        FScriptArrayHelper helper{array_property, array_property->ContainerPtrToValuePtr<void>(panel)};
        for (int32_t i = 0; i < helper.Num(); ++i)
        {
            auto* slot = get_array_object_element(array_property, panel, i);
            auto* content = get_object_property_value(slot, STR("Content"));
            if (is_suit_grid_button(content) &&
                widget_has_direct_menu_signal(content))
            {
                return true;
            }
        }

        return false;
    }


    bool suit_grid_is_safe_for_auto_use(
        UObject* suit_menu,
        UObject* options_grid,
        const int32_t buttons)
    {
        // rf85: any plausible-count Batman grid (not just exactly 43) counts as a
        // clean native grid -- the Batman-character gate now ensures it is Batman's
        // menu, so the suit count can vary with the player's unlocks.
        const bool clean_native_grid =
            buttons >= 20 &&
            buttons <= 80;

        if (clean_native_grid)
        {
            return true;
        }

        const uint64_t now =
            GetTickCount64();

        const bool recently_injected_same_grid =
            options_grid &&
            SuitSlotState.LastInjectedOptionsGrid == options_grid &&
            SuitSlotState.LastSuccessfulGridInjectionMs > 0 &&
            now <
                SuitSlotState.LastSuccessfulGridInjectionMs + 2500ULL;

        if (recently_injected_same_grid)
        {
            return true;
        }

        return suit_grid_has_direct_menu_signal(
            suit_menu,
            options_grid);
    }


    bool suit_grid_should_block_gameplay_or_handoff(
        UObject* suit_menu,
        UObject* options_grid,
        const int32_t buttons)
    {
        if (!suit_menu ||
            !options_grid ||
            buttons < 20 ||
            buttons > 80)
        {
            return false;
        }

        // rf138: find_ready_batman_suit_grid intentionally remains permissive
        // for injection/reopen discovery, because IsInViewport is unreliable in
        // this build. Gameplay apply and source-pawn handoff need a stricter
        // definition, though: old W_CharacterSuitMenu/OptionsGrid widgets can
        // remain live in memory after the player is back in Gotham. Those stale
        // widgets must not keep F5/auto-handoff/gameplay cosmetics waiting.
        if (suit_grid_has_direct_menu_signal(
                suit_menu,
                options_grid))
        {
            return true;
        }

        const uint64_t now =
            GetTickCount64();

        const bool recent_same_grid_injection =
            SuitSlotState.LastInjectedOptionsGrid == options_grid &&
            SuitSlotState.LastSuccessfulGridInjectionMs > 0 &&
            now <
                SuitSlotState.LastSuccessfulGridInjectionMs + 2500ULL;

        if (recent_same_grid_injection)
        {
            return true;
        }

        return
            SuitSlotState.PreviewStableHoverArmed ||
            SuitSlotState.PreviewStableDirectReinforceArmed ||
            PreviewHoverCaptureArmed.load() ||
            CapturedPreviewApplyPending.load() ||
            CustomSlotHoverPending.load() ||
            CustomSlotClickPending.load() ||
            PendingCustomSuitIndex.load() >= 0 ||
            SuitSlotState.PendingPreviewApplySuitIndex >= 0 ||
            SuitSlotState.PendingMenuTextApplyFrames > 0 ||
            SuitSlotState.PendingSubMenuTextApplyFrames > 0 ||
            SuitSlotState.CustomButtonTextGuardFrames > 0 ||
            SuitSlotState.SelectionVisualGuardFrames > 0 ||
            (SuitSlotState.MenuGenerationSettlingDeadlineMs > 0 &&
             now < SuitSlotState.MenuGenerationSettlingDeadlineMs) ||
            (SuitSlotState.CustomHoverNativeNoiseGuardDeadlineMs > 0 &&
             now < SuitSlotState.CustomHoverNativeNoiseGuardDeadlineMs) ||
            (SuitSlotState.CustomCommitAliasGuardDeadlineMs > 0 &&
             now < SuitSlotState.CustomCommitAliasGuardDeadlineMs);
    }


    UObject* kismet_load_asset_blocking(const wchar_t* path, StringViewType label);
    UObject* kismet_load_class_asset_blocking(const wchar_t* path,
                                               StringViewType label);
    bool call_function_soft_object_path_param_void(UObject* object,
                                                   const wchar_t* function_name,
                                                   const wchar_t* soft_object_path,
                                                   StringViewType label);
    bool apply_json_uimd_icon_overrides(const VisualSlotJsonConfig& config);
    bool queue_json_uimd_icon_override(int64_t config_index,
                                       const VisualSlotJsonConfig& config,
                                       StringViewType reason);
    void tick_deferred_uimd_icon_overrides();
    bool apply_spawn_time_uimd_icon_overrides_for_actor(UObject* actor, StringViewType reason);
    void arm_hud_player_portrait_override_for_actor(UObject* actor, int64_t config_index, StringViewType reason);

    bool try_apply_visual_slot_icon(UObject* new_button, const VisualSlotJsonConfig& config);
    void install_visual_click_flow_hooks(UObject* preferred_button = nullptr,
                                         UObject* preferred_suit_menu = nullptr);
    bool visual_click_flow_required_hooks_installed();
    bool call_function_bool_return_no_params(UObject* object, const wchar_t* function_name, bool& out_value);
    void log_visual_slot_selection_probe(UObject* suit_menu,
                                         UObject* options_grid,
                                         UObject* new_button,
                                         const VisualSlotJsonConfig& config,
                                         StringViewType phase);

    StringType extract_exported_gameplay_tag_name(StringViewType exported_tag)
    {
        const StringType text{exported_tag};
        const StringType marker = STR("TagName=\"");
        const auto marker_pos = text.find(marker);
        if (marker_pos == StringType::npos)
        {
            return {};
        }

        const auto value_start = marker_pos + marker.size();
        const auto value_end = text.find(STR("\""), value_start);
        if (value_end == StringType::npos || value_end <= value_start)
        {
            return {};
        }

        return text.substr(value_start, value_end - value_start);
    }

    StringType get_suit_button_gameplay_tag_name(UObject* button)
    {
        if (!button)
        {
            return {};
        }

        const auto exported_tag =
            call_function_return_text(button, STR("GetTag"));

        auto tag_name =
            extract_exported_gameplay_tag_name(exported_tag);

        if (!tag_name.empty())
        {
            return tag_name;
        }

        // Defensive fallback: a few widget paths stringify tags differently in
        // logs. If the bridge tag is plainly present, treat it as exact enough
        // for restore gating instead of restoring on every native hover.
        if (exported_tag.find(NativeSuitBridgeSourceTag) != StringType::npos)
        {
            return NativeSuitBridgeSourceTag;
        }

        if (exported_tag.find(NativeSuitBridgeAltSourceTag) != StringType::npos)
        {
            return NativeSuitBridgeAltSourceTag;
        }

        return {};
    }


    bool is_batman_playable_actor(UObject* object);

    // rf85: only operate on BATMAN's suit menu. The W_CharacterSuitMenu widget is
    // shared across characters, so gate everything on the last-possessed gameplay
    // pawn being a Batman playable -- otherwise we'd inject Batman suits into
    // another character's menu. (The possession hook tracks the current character;
    // future multi-character support will parameterize this instead of hard-gating
    // on Batman.)
    bool current_suit_menu_character_is_batman()
    {
        auto* cached =
            SuitSlotState.LastPossessedGameplayPawn;

        return cached &&
               is_live_uobject_pointer(cached) &&
               is_batman_playable_actor(cached);
    }


    bool find_ready_batman_suit_grid(
        UObject*& out_suit_menu,
        UObject*& out_options_grid,
        int32_t& out_slots,
        int32_t& out_buttons,
        const bool require_current_menu_signal = false)
    {
        out_suit_menu = nullptr;
        out_options_grid = nullptr;
        out_slots = 0;
        out_buttons = 0;

        // rf85: Batman's suit menu only.
        if (!current_suit_menu_character_is_batman())
        {
            return false;
        }

        const int32_t expected_buttons_after_previous_injection =
            SuitSlotState.LastInjectedBaseButtons > 0 &&
                    SuitSlotState.LastInjectedCount > 0
                ? SuitSlotState.LastInjectedBaseButtons +
                      SuitSlotState.LastInjectedCount
                : -1;

        UObjectGlobals::ForEachUObject([&](UObject* object, ...) -> LoopAction {
            if (!object || !is_probable_runtime_instance(object))
            {
                return LoopAction::Continue;
            }

            auto* object_class = object->GetClassPrivate();
            const auto full_name = object->GetFullName();
            const auto class_name = object_class ? object_class->GetFullName() : STR("");
            const auto combined = full_name + STR(" ") + class_name;
            if (!is_transient_runtime_object(object) ||
                (!contains(combined, STR("W_CharacterSuitMenu_C")) && !contains(combined, STR("CharacterSuitMenu"))))
            {
                return LoopAction::Continue;
            }

            // Do not gate menu discovery on UUserWidget::IsInViewport here.
            // In this shipping build the reflected bool result is unreliable
            // and reports the active suit menu as false, preventing injection.
            // 0.11.4v4g: accept either the clean native grid or a reopened grid
            // that still contains the previously cloned custom buttons, then
            // rebind/adopt rather than duplicate-injecting.

            auto* options_grid = get_object_property_value(object, STR("OptionsGrid"));
            const auto slots = get_array_property_count(options_grid, STR("Slots"));
            const auto buttons = count_suit_grid_buttons(options_grid);
            if (options_grid &&
                slots == buttons &&
                buttons >= 20 &&
                buttons <= 80)
            {
                if (require_current_menu_signal &&
                    !suit_grid_is_safe_for_auto_use(
                        object,
                        options_grid,
                        buttons))
                {
                    return LoopAction::Continue;
                }

                // 0.11.4v4y reopen fix: after backing out and reopening the menu
                // within the grid-missing grace window, the OLD menu (still
                // holding the previously cloned custom buttons) and the NEW
                // visible menu can both exist in memory. Returning the first one
                // we happen to iterate can pick the STALE old grid, whose 51-button
                // count makes the injection gate "adopt-existing-clones" and skip
                // injecting into the new menu -> custom slots appear missing on
                // reopen. So for the injection path, prefer the grid the player is
                // actually looking at (the one with a live menu focus/hover
                // signal). Keep the first match only as a fallback; keep scanning
                // for a focused one. Other callers (require_current_menu_signal
                // =false) keep the original first-match behaviour.
                if (!require_current_menu_signal)
                {
                    out_suit_menu = object;
                    out_options_grid = options_grid;
                    out_slots = slots;
                    out_buttons = buttons;
                    return LoopAction::Break;
                }

                const bool grid_has_focus_signal =
                    suit_grid_has_direct_menu_signal(
                        object,
                        options_grid);

                if (grid_has_focus_signal)
                {
                    out_suit_menu = object;
                    out_options_grid = options_grid;
                    out_slots = slots;
                    out_buttons = buttons;
                    return LoopAction::Break;
                }

                if (!out_suit_menu)
                {
                    // First safe-but-unfocused candidate: remember as fallback,
                    // but keep looking for the focused (truly visible) menu.
                    out_suit_menu = object;
                    out_options_grid = options_grid;
                    out_slots = slots;
                    out_buttons = buttons;
                }
                else
                {
                    const bool candidate_is_previous_grid =
                        options_grid ==
                        SuitSlotState.LastInjectedOptionsGrid;

                    const bool fallback_is_previous_grid =
                        out_options_grid ==
                        SuitSlotState.LastInjectedOptionsGrid;

                    const bool candidate_has_previous_clone_count =
                        expected_buttons_after_previous_injection > 0 &&
                        buttons >=
                            expected_buttons_after_previous_injection;

                    const bool fallback_has_previous_clone_count =
                        expected_buttons_after_previous_injection > 0 &&
                        out_buttons >=
                            expected_buttons_after_previous_injection;

                    if ((fallback_is_previous_grid &&
                         !candidate_is_previous_grid) ||
                        (fallback_has_previous_clone_count &&
                         !candidate_has_previous_clone_count))
                    {
                        out_suit_menu = object;
                        out_options_grid = options_grid;
                        out_slots = slots;
                        out_buttons = buttons;
                    }
                }

                return LoopAction::Continue;
            }

            return LoopAction::Continue;
        });

        return out_suit_menu && out_options_grid;
    }

    // Defined later alongside the rf236 pawn-metadata diagnostics. The census
    // tick lives here at the grid-discovery boundary, so it needs this forward
    // declaration before dispatching the completed stable-grid snapshot.
    void run_native_suit_menu_census(
        UObject* suit_menu,
        UObject* options_grid,
        int32_t reported_slots,
        int32_t reported_buttons);


    // Defined just below; auto-inject reuses this proven manual path.
    // rf204: the optional known_* parameters let a caller that has ALREADY resolved the live
    // suit grid hand it straight over, skipping find_ready_batman_suit_grid's global UObject
    // scan. Omitted (F6/F7) = find from scratch, exactly as before.
    void manual_inject_visual_suit_grid_slots(
        StringViewType key_label,
        UObject* known_suit_menu = nullptr,
        UObject* known_options_grid = nullptr,
        int32_t known_slots = 0,
        int32_t known_buttons = 0);


    struct RawUtf8StringParam
    {
        const char* Data{};
        int32_t Len{};
        int32_t Capacity{};
    };

    static_assert(sizeof(RawUtf8StringParam) == 0x10, "FUtf8String param layout changed.");

    struct RawSoftObjectPtrParam
    {
        int32_t ObjectIndex{-1};
        int32_t ObjectSerialNumber{};
        FName PackageName{};
        FName AssetName{};
        RawUtf8StringParam SubPathString{};
    };

    static_assert(sizeof(RawSoftObjectPtrParam) == 0x28, "TSoftObjectPtr<UObject> param layout changed.");

    // UE 5.6 FPrimaryAssetId is two FNames: the primary asset type followed by
    // the primary asset name. Keep this local/raw just like RawSoftObjectPtrParam
    // so command probes can use reflected Kismet helpers without depending on a
    // generated SDK header at build time.
    struct RawPrimaryAssetIdParam
    {
        FName PrimaryAssetType{};
        FName PrimaryAssetName{};
    };

    static_assert(sizeof(RawPrimaryAssetIdParam) == 0x10, "FPrimaryAssetId param layout changed.");

    // UE 5.6 FSoftObjectPath is an FTopLevelAssetPath (two FNames) followed
    // by its UTF-8 subobject string. This is the path form accepted by the
    // native UAssetManager API; it intentionally does not contain the weak
    // UObject index/serial prefix used by RawSoftObjectPtrParam above.
    struct RawSoftObjectPathParam
    {
        FName PackageName{};
        FName AssetName{};
        RawUtf8StringParam SubPathString{};
    };

    static_assert(sizeof(RawSoftObjectPathParam) == 0x20, "FSoftObjectPath param layout changed.");

    // FAssetBundleData is a single TArray<FAssetBundleEntry>. The first
    // registration experiment deliberately supplies an empty bundle table;
    // PawnMetaData only needs its primary ID -> object path mapping here.
    struct RawAssetBundleDataParam
    {
        void* Data{};
        int32_t Num{};
        int32_t Capacity{};
    };

    static_assert(sizeof(RawAssetBundleDataParam) == 0x10, "FAssetBundleData param layout changed.");

    bool split_soft_object_path(const wchar_t* path, std::wstring& package_name, std::wstring& asset_name)
    {
        if (!path || !*path)
        {
            return false;
        }

        std::wstring object_path{path};
        if (const auto subobject_delimiter = object_path.find(L':'); subobject_delimiter != std::wstring::npos)
        {
            object_path.resize(subobject_delimiter);
        }

        const auto asset_delimiter = object_path.rfind(L'.');
        if (asset_delimiter == std::wstring::npos || asset_delimiter == 0 || asset_delimiter + 1 >= object_path.size())
        {
            return false;
        }

        package_name = object_path.substr(0, asset_delimiter);
        asset_name = object_path.substr(asset_delimiter + 1);
        return !package_name.empty() && !asset_name.empty();
    }

    // rf312: Blueprint generated-class paths are TSoftClassPtr values, not
    // TSoftObjectPtr<UObject> values. LoadAsset_Blocking accepted our raw bytes
    // but returned null for every custom playable/cutscene "_C" path. UE 5.6
    // exposes the matching LoadClassAsset_Blocking entry point with the same
    // 0x28 soft-pointer payload followed by a UClass* return value.
    UObject* kismet_load_class_asset_blocking(const wchar_t* path,
                                              StringViewType label)
    {
        auto* kismet =
            find_loaded_object(
                STR("/Script/Engine.Default__KismetSystemLibrary"));
        auto* function =
            kismet
                ? kismet->GetFunctionByNameInChain(
                      STR("LoadClassAsset_Blocking"))
                : nullptr;
        if (!kismet || !function)
        {
            suit_vlog(
                STR("{}   {} Kismet LoadClassAsset_Blocking unavailable kismet={} function={}\n"),
                ModTag,
                label,
                kismet && is_live_uobject_pointer(kismet)
                    ? kismet->GetFullName()
                    : STR("<none-or-stale>"),
                function && is_live_uobject_pointer(function)
                    ? function->GetFullName()
                    : STR("<none-or-stale>"));
            return nullptr;
        }

        struct LoadClassAssetBlocking_Params
        {
            RawSoftObjectPtrParam AssetClass{};
            UObject* ReturnValue{};
        };

        const auto param_size =
            std::max<int32_t>(
                function->GetPropertiesSize(),
                function->GetStructureSize());
        if (param_size <= 0 ||
            static_cast<size_t>(param_size) >
                sizeof(LoadClassAssetBlocking_Params))
        {
            suit_vlog(
                STR("{}   {} Kismet LoadClassAsset_Blocking size mismatch function_size={} local_size={} path={}\n"),
                ModTag,
                label,
                param_size,
                sizeof(LoadClassAssetBlocking_Params),
                path);
            return nullptr;
        }

        std::wstring package_name;
        std::wstring asset_name;
        if (!split_soft_object_path(path, package_name, asset_name))
        {
            suit_vlog(
                STR("{}   {} Kismet LoadClassAsset_Blocking invalid soft class path={}\n"),
                ModTag,
                label,
                path);
            return nullptr;
        }

        LoadClassAssetBlocking_Params params{};
        params.AssetClass.PackageName =
            FName{package_name.c_str(), FNAME_Add};
        params.AssetClass.AssetName =
            FName{asset_name.c_str(), FNAME_Add};
        kismet->ProcessEvent(function, &params);

        const bool result_live =
            params.ReturnValue &&
            is_live_uobject_pointer(params.ReturnValue);
        suit_vlog(
            STR("{}   {} Kismet LoadClassAsset_Blocking path={} result={}\n"),
            ModTag,
            label,
            path,
            result_live
                ? params.ReturnValue->GetFullName()
                : STR("<null-or-stale>"));
        return result_live
                   ? params.ReturnValue
                   : nullptr;
    }

    // Object counterpart to the class loader above for generic PrimaryDataAssets.
    UObject* kismet_load_asset_blocking(const wchar_t* path,
                                        StringViewType label)
    {
        auto* kismet =
            find_loaded_object(
                STR("/Script/Engine.Default__KismetSystemLibrary"));
        auto* function =
            kismet
                ? kismet->GetFunctionByNameInChain(
                      STR("LoadAsset_Blocking"))
                : nullptr;
        if (!kismet || !function)
        {
            suit_vlog(
                STR("{}   {} Kismet LoadAsset_Blocking unavailable\n"),
                ModTag,
                label);
            return nullptr;
        }

        struct LoadAssetBlocking_Params
        {
            RawSoftObjectPtrParam Asset{};
            UObject* ReturnValue{};
        };

        const auto param_size =
            std::max<int32_t>(
                function->GetPropertiesSize(),
                function->GetStructureSize());
        if (param_size <= 0 ||
            static_cast<size_t>(param_size) >
                sizeof(LoadAssetBlocking_Params))
        {
            suit_vlog(
                STR("{}   {} Kismet LoadAsset_Blocking size mismatch function_size={} local_size={} path={}\n"),
                ModTag,
                label,
                param_size,
                sizeof(LoadAssetBlocking_Params),
                path);
            return nullptr;
        }

        std::wstring package_name;
        std::wstring asset_name;
        if (!split_soft_object_path(path, package_name, asset_name))
        {
            suit_vlog(
                STR("{}   {} Kismet LoadAsset_Blocking invalid soft object path={}\n"),
                ModTag,
                label,
                path);
            return nullptr;
        }

        LoadAssetBlocking_Params params{};
        params.Asset.PackageName =
            FName{package_name.c_str(), FNAME_Add};
        params.Asset.AssetName =
            FName{asset_name.c_str(), FNAME_Add};
        kismet->ProcessEvent(function, &params);

        return params.ReturnValue &&
               is_live_uobject_pointer(params.ReturnValue)
            ? params.ReturnValue
            : nullptr;
    }

    struct DebugMenuAssetProbeResult
    {
        UObject* RegistryLoaded{};
        UObject* BlockingLoaded{};
        UObject* StaticAfter{};
        bool RegistryLiveFound{};
        bool RegistryDiskFound{};

        UObject* best_loaded_object() const
        {
            if (BlockingLoaded)
            {
                return BlockingLoaded;
            }
            if (RegistryLoaded)
            {
                return RegistryLoaded;
            }
            return StaticAfter;
        }
    };

    // Defined later with the registry diagnostics. Character roster exports can
    // be very large, so the read-only roster census uses the same bounded logger.
    StringType truncate_registry_probe_text(
        StringViewType text,
        size_t max_chars);

    bool is_source_actor_for_config(
        UObject* object,
        const VisualSlotJsonConfig& config);

    bool is_batman_playable_actor(UObject* object);

    UObject* first_object_input_parameter(
        UFunction* function,
        void* params);

    void observe_event_driven_saved_cutscene_activation(
        UObject* context,
        UFunction* function);

    StringType gameplay_world_key_from_object_name(
        StringViewType full_name)
    {
        const auto start =
            full_name.find(STR("/Game/Levels/"));

        const auto end =
            full_name.find(STR(":PersistentLevel."));

        if (start == StringViewType::npos ||
            end == StringViewType::npos ||
            end <= start ||
            contains(full_name, STR("/Game/Levels/Frontend/")))
        {
            return {};
        }

        return StringType{
            full_name.substr(
                start,
                end - start)};
    }


    StringType truncate_registry_probe_text(
        StringViewType text,
        const size_t max_chars)
    {
        if (text.size() <= max_chars)
        {
            return StringType{text};
        }

        return StringType{
                   text.substr(
                       0,
                       max_chars)} +
            STR("...<truncated>");
    }


    UObject* find_live_tt_pawn_metadata_system()
    {
        UObject* result = nullptr;
        int32_t candidate_count = 0;

        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                if (!object ||
                    result)
                {
                    return LoopAction::Continue;
                }

                const auto full_name =
                    object->GetFullName();

                const bool is_type_object =
                    starts_with(full_name, STR("Class ")) ||
                    starts_with(full_name, STR("Package ")) ||
                    starts_with(full_name, STR("Function ")) ||
                    starts_with(full_name, STR("ScriptStruct ")) ||
                    starts_with(full_name, STR("Enum ")) ||
                    starts_with(full_name, STR("Blueprint ")) ||
                    starts_with(full_name, STR("BlueprintGeneratedClass ")) ||
                    starts_with(full_name, STR("WidgetBlueprintGeneratedClass "));

                if (is_type_object ||
                    contains(full_name, STR("Default__")))
                {
                    return LoopAction::Continue;
                }

                auto* object_class =
                    object->GetClassPrivate();

                const auto class_name =
                    object_class
                        ? object_class->GetFullName()
                        : StringType{};

                const bool class_match =
                    contains(class_name, STR("TtPawnMetaDataSystem"));

                const bool name_match =
                    contains(full_name, STR("TtPawnMetaDataSystem"));

                if (class_match ||
                    name_match)
                {
                    ++candidate_count;
                }

                if (class_match)
                {
                    result = object;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        if (!result)
        {
            suit_vlog(
                STR("{} SuitRuntime PAWN_METADATA_REGISTRY_SYSTEM_FIND result=<null> candidate_count={} policy=skip-type-objects-require-live-instance\n"),
                ModTag,
                candidate_count);
        }

        return result;
    }


    struct PawnTagContainerSnapshot
    {
        bool Called{};
        bool LayoutValid{};
        int32_t GameplayTagCount{};
        int32_t ParentTagCount{};
        std::vector<StringType> GameplayTags{};
        StringType Failure{};
    };


    UObject* call_tt_pawn_metadata_system_get_metadata_for_tag(
        UObject* metadata_system,
        StringViewType pawn_tag,
        StringViewType reason)
    {
        auto* function =
            metadata_system
                ? metadata_system->GetFunctionByNameInChain(
                      STR("GetMetaDataForPawnTag"))
                : nullptr;

        if (!metadata_system ||
            !function ||
            pawn_tag.empty())
        {
            suit_vlog(
                STR("{} SuitRuntime PAWN_METADATA_REGISTRY_QUERY_SKIPPED system={} pawn_tag={} reason={} failure=missing-system-function-or-tag\n"),
                ModTag,
                metadata_system
                    ? metadata_system->GetFullName()
                    : STR("<none>"),
                pawn_tag.empty()
                    ? STR("<none>")
                    : pawn_tag,
                reason.empty()
                    ? STR("<none>")
                    : reason);
            return nullptr;
        }

        const auto param_size =
            std::max<int32_t>(
                function->GetPropertiesSize(),
                function->GetStructureSize());

        UObject* result = nullptr;

        if (param_size == 0x10)
        {
            alignas(8) std::array<uint8_t, 0x10> params{};

            const FName tag_name{
                StringType{pawn_tag}.c_str(),
                FNAME_Add};

            std::memcpy(
                params.data(),
                &tag_name,
                sizeof(FName));

            const bool previous_internal =
                GlobalSelectionProcessEventTraceInternal;

            GlobalSelectionProcessEventTraceInternal = true;

            const uint32_t previous_function_flags =
                function->GetFunctionFlags();

            function->GetFunctionFlags() =
                previous_function_flags | 0x00000400u;

            metadata_system->ProcessEvent(
                function,
                params.data());

            function->GetFunctionFlags() =
                previous_function_flags;

            GlobalSelectionProcessEventTraceInternal =
                previous_internal;

            std::memcpy(
                &result,
                params.data() + 0x08,
                sizeof(result));
        }
        else if (param_size > 0 &&
                 param_size <= 256)
        {
            std::vector<uint8_t> params(
                static_cast<size_t>(param_size),
                0);

            function->InitializeStruct(
                params.data());

            FProperty* tag_property = nullptr;
            FObjectPropertyBase* return_property = nullptr;

            for (auto* property :
                 TFieldRange<FProperty>(
                     function,
                     EFieldIterationFlags::IncludeDeprecated))
            {
                if (!property ||
                    !property->HasAnyPropertyFlags(CPF_Parm))
                {
                    continue;
                }

                if (property->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    return_property =
                        CastField<FObjectPropertyBase>(
                            property);
                    continue;
                }

                if (!tag_property &&
                    contains(property->GetName(), STR("Tag")))
                {
                    tag_property = property;
                }
            }

            if (tag_property &&
                return_property)
            {
                const FName tag_name{
                    StringType{pawn_tag}.c_str(),
                    FNAME_Add};

                auto* destination =
                    tag_property->ContainerPtrToValuePtr<void>(
                        params.data());

                if (destination)
                {
                    std::memcpy(
                        destination,
                        &tag_name,
                        sizeof(FName));
                }

                const bool previous_internal =
                    GlobalSelectionProcessEventTraceInternal;

                GlobalSelectionProcessEventTraceInternal = true;

                const uint32_t previous_function_flags =
                    function->GetFunctionFlags();

                function->GetFunctionFlags() =
                    previous_function_flags | 0x00000400u;

                metadata_system->ProcessEvent(
                    function,
                    params.data());

                function->GetFunctionFlags() =
                    previous_function_flags;

                GlobalSelectionProcessEventTraceInternal =
                    previous_internal;

                result =
                    return_property->GetObjectPropertyValue(
                        return_property->ContainerPtrToValuePtr<void>(
                            params.data()));
            }

            function->DestroyStruct(
                params.data());
        }

        if (result &&
            !is_live_uobject_pointer(result))
        {
            result = nullptr;
        }

        return result;
    }


    struct RawScriptArrayHeader
    {
        uintptr_t Data{};
        int32_t Num{};
        int32_t Max{};
    };

    static_assert(
        sizeof(RawScriptArrayHeader) == 0x10,
        "RawScriptArrayHeader must match the 64-bit TArray header size.");


    bool call_function_bool_return_no_params(UObject* object, const wchar_t* function_name, bool& out_value)
    {
        out_value = false;
        auto* function = object ? object->GetFunctionByNameInChain(function_name) : nullptr;
        auto* return_property = function ? CastField<FBoolProperty>(function->GetReturnProperty()) : nullptr;
        if (!object || !function || !return_property)
        {
            return false;
        }

        const auto param_size = std::max<int32_t>(function->GetPropertiesSize(), function->GetStructureSize());
        if (param_size < 0 || param_size > 1024)
        {
            return false;
        }

        std::vector<uint8_t> params(static_cast<size_t>(std::max<int32_t>(param_size, 1)), 0);
        function->InitializeStruct(params.data());
        object->ProcessEvent(function, params.data());
        out_value = return_property->GetPropertyValue(return_property->ContainerPtrToValuePtr<void>(params.data()));
        function->DestroyStruct(params.data());
        return true;
    }


    // Defined later with the source-metadata preview alias helpers.
    // Required here by the caller-stack/burst tracing helpers.


    void observe_thomas_source_skin_actor_from_process_event(
        UObject* context,
        UFunction* function);


    void tick_thomas_source_skin_asset_preload_burst(
        const int32_t max_steps);


    bool is_native_suit_selection_event_name(
        StringViewType event_name)
    {
        // BP_OnPressed is emitted before the menu has accepted the new
        // selection and may also be synthesized while focus moves between
        // buttons. Persist only the completed Clicked notification, then
        // verify the button's selected state in the matching post callback.
        return event_name == STR("BP_OnClicked");
    }


    bool is_native_suit_button_generation_event_name(
        StringViewType event_name)
    {
        // SetupButton/construct calls are the only periods where the native
        // character menu is allowed to synthesize button events. Do not treat
        // SetCurrentSelection/BP_OnSelected as generation: those also occur as
        // part of a legitimate accepted click.
        return event_name == STR("SetupButton") ||
            event_name == STR("PreConstruct") ||
            event_name == STR("Construct");
    }


    bool native_suit_selection_tag_is_readable(
        StringViewType pawn_tag)
    {
        return !pawn_tag.empty() &&
            pawn_tag != STR("<missing function>") &&
            pawn_tag != STR("<no return property>");
    }


    struct ScopedNativeSuitSelectionProbeGuards
    {
        bool PreviousSelectionGuard;
        bool PreviousNativeGuard;

        ScopedNativeSuitSelectionProbeGuards()
            : PreviousSelectionGuard(
                  GlobalSelectionProcessEventTraceInternal),
              PreviousNativeGuard(
                  NativeManifestProcessEventInternal)
        {
            GlobalSelectionProcessEventTraceInternal = true;
            NativeManifestProcessEventInternal = true;
        }

        ~ScopedNativeSuitSelectionProbeGuards()
        {
            NativeManifestProcessEventInternal = PreviousNativeGuard;
            GlobalSelectionProcessEventTraceInternal =
                PreviousSelectionGuard;
        }
    };


    void reset_pending_native_suit_selection_confirmation()
    {
        std::lock_guard<std::mutex> lock{
            PendingNativeSuitSelectionConfirmationMutex};
        ++PendingNativeSuitSelectionConfirmationGeneration;
        PendingNativeSuitSelectionConfirmationState = {};
        PendingNativeSuitSelectionConfirmationValid = false;
        NativeSuitSelectionConfirmationPending.store(
            false,
            std::memory_order_release);
    }


    void queue_pending_native_suit_selection_confirmation(
        UObject* suit_menu,
        StringViewType pawn_tag,
        StringViewType event_name)
    {
        if (!suit_menu ||
            !native_suit_selection_tag_is_readable(pawn_tag))
        {
            return;
        }

        constexpr uint64_t initial_poll_delay_ms = 25ULL;
        constexpr uint64_t confirmation_deadline_ms = 4000ULL;
        const uint64_t now = GetTickCount64();

        PendingNativeSuitSelectionConfirmation candidate{};
        candidate.SuitMenu = suit_menu;
        candidate.SuitMenuName = suit_menu->GetFullName();
        candidate.PawnTag = pawn_tag;
        candidate.EventName = event_name;
        candidate.QueuedAtMs = now;
        candidate.NextPollMs = now + initial_poll_delay_ms;
        candidate.DeadlineMs = now + confirmation_deadline_ms;

        {
            std::lock_guard<std::mutex> lock{
                PendingNativeSuitSelectionConfirmationMutex};
            candidate.Generation =
                ++PendingNativeSuitSelectionConfirmationGeneration;
            PendingNativeSuitSelectionConfirmationState =
                std::move(candidate);
            PendingNativeSuitSelectionConfirmationValid = true;
            NativeSuitSelectionConfirmationPending.store(
                true,
                std::memory_order_release);
        }

        suit_vlog(
            STR("{} Queued authoritative suit selection confirmation: menu={} pawn_tag={} event={} deadline_ms={}\n"),
            ModTag,
            suit_menu->GetFullName(),
            pawn_tag,
            event_name,
            confirmation_deadline_ms);
    }


    void tick_pending_native_suit_selection_confirmation()
    {
        if (!NativeSuitSelectionConfirmationPending.load(
                std::memory_order_acquire))
        {
            return;
        }

        constexpr uint64_t poll_interval_ms = 25ULL;
        SelectionTiming timing{L"confirm-selection"};
        const uint64_t now = GetTickCount64();
        PendingNativeSuitSelectionConfirmation candidate{};
        bool expired = false;

        {
            std::lock_guard<std::mutex> lock{
                PendingNativeSuitSelectionConfirmationMutex};
            if (!PendingNativeSuitSelectionConfirmationValid)
            {
                NativeSuitSelectionConfirmationPending.store(
                    false,
                    std::memory_order_release);
                return;
            }

            if (now <
                PendingNativeSuitSelectionConfirmationState.NextPollMs)
            {
                return;
            }

            candidate =
                PendingNativeSuitSelectionConfirmationState;
            if (now >= candidate.DeadlineMs)
            {
                PendingNativeSuitSelectionConfirmationState = {};
                PendingNativeSuitSelectionConfirmationValid = false;
                NativeSuitSelectionConfirmationPending.store(
                    false,
                    std::memory_order_release);
                expired = true;
            }
            else
            {
                PendingNativeSuitSelectionConfirmationState.NextPollMs =
                    now + poll_interval_ms;
                ++PendingNativeSuitSelectionConfirmationState.PollCount;
                candidate.PollCount =
                    PendingNativeSuitSelectionConfirmationState.PollCount;
            }
        }

        if (expired)
        {
            suit_log(
                STR("{} Suit selection confirmation expired without an authoritative menu match; existing saved selection was preserved menu={} pawn_tag={} event={} polls={} elapsed_ms={}\n"),
                ModTag,
                candidate.SuitMenuName,
                candidate.PawnTag,
                candidate.EventName,
                candidate.PollCount,
                now - candidate.QueuedAtMs);
            return;
        }

        auto* suit_menu = candidate.SuitMenu.Get();
        if (!suit_menu ||
            !is_probable_runtime_instance(suit_menu) ||
            !is_transient_runtime_object(suit_menu) ||
            suit_menu->GetFullName() != candidate.SuitMenuName ||
            !class_name_contains(
                suit_menu,
                STR("/Game/UI/GameMenu/CharacterSelect/W_CharacterSuitMenu.W_CharacterSuitMenu_C")))
        {
            // The menu normally remains alive long enough to publish its selected
            // option. If it vanishes first, retain the existing sidecar and let the
            // bounded deadline close this candidate; never dereference a stale UI
            // object or infer acceptance from possession alone.
            return;
        }

        StringType selected_pawn_tag{};
        {
            ScopedNativeSuitSelectionProbeGuards probe_guards{};
            selected_pawn_tag =
                extract_exported_gameplay_tag_name(
                    call_function_return_text(
                        suit_menu,
                        STR("GetSelectedOption")));
        }

        if (!native_suit_selection_tag_is_readable(
                selected_pawn_tag) ||
            !tag_identity_equals(
                selected_pawn_tag,
                candidate.PawnTag))
        {
            return;
        }

        // Claim exactly the generation we observed. A newer click always wins,
        // even if this menu call caused nested callbacks before it returned.
        {
            std::lock_guard<std::mutex> lock{
                PendingNativeSuitSelectionConfirmationMutex};
            if (!PendingNativeSuitSelectionConfirmationValid ||
                PendingNativeSuitSelectionConfirmationState.Generation !=
                    candidate.Generation)
            {
                return;
            }

            PendingNativeSuitSelectionConfirmationState = {};
            PendingNativeSuitSelectionConfirmationValid = false;
            NativeSuitSelectionConfirmationPending.store(
                false,
                std::memory_order_release);
        }

        suit_vlog(
            STR("{} Authoritative suit selection confirmed: menu={} pawn_tag={} event={} polls={} elapsed_ms={}\n"),
            ModTag,
            candidate.SuitMenuName,
            selected_pawn_tag,
            candidate.EventName,
            candidate.PollCount,
            now - candidate.QueuedAtMs);

        const NativeSuitModManifest* manifest = nullptr;
        const auto* manifest_suit =
            find_enabled_native_manifest_suit_by_pawn_tag(
                selected_pawn_tag,
                manifest);

        if (manifest && manifest_suit)
        {
            write_saved_native_manifest_selection(
                *manifest,
                *manifest_suit,
                candidate.EventName);
        }
        else
        {
            clear_saved_native_manifest_selection(
                derive_native_manifest_character_scope(
                    selected_pawn_tag),
                candidate.EventName);
        }
    }


    void global_selection_process_event_trace_pre(
        UObject* context,
        UFunction* function,
        void* params)
    {
        (void)params;
        if (!NativeSchema3ManifestModeActive ||
            !context ||
            !function)
        {
            return;
        }

        if (!(ExpandedEvents::flags(function) & (ExpandedEvents::Selection | ExpandedEvents::Generation))) return;
        SelectionTiming timing{L"selection-pre-observer"};
        const auto event_name = function->GetName();
        if (is_native_suit_button_generation_event_name(event_name) &&
            is_suit_grid_button(context))
        {
            ++NativeSuitSelectionMenuGenerationDepth;
            return;
        }

        if (GlobalSelectionProcessEventTraceInternal ||
            NativeManifestProcessEventInternal ||
            !is_native_suit_selection_event_name(event_name) ||
            !is_suit_grid_button(context))
        {
            return;
        }

        PendingNativeSuitSelectionEvent observation{};
        observation.Button = context;
        observation.Function = function;
        observation.EventName = event_name;

        const uint64_t now = GetTickCount64();
        observation.MenuGenerationSettling =
            NativeSuitSelectionMenuGenerationDepth > 0 ||
            (SuitSlotState.MenuGenerationSettlingDeadlineMs > 0 &&
             now < SuitSlotState.MenuGenerationSettlingDeadlineMs);

        if (!observation.MenuGenerationSettling)
        {
            ScopedNativeSuitSelectionProbeGuards probe_guards{};
            auto* suit_menu =
                find_live_suit_menu_containing_button(context);
            observation.LiveMenuSignal =
                suit_menu &&
                widget_has_direct_menu_signal(context);
            if (observation.LiveMenuSignal)
            {
                observation.SuitMenu = suit_menu;
                observation.PawnTag =
                    get_suit_button_gameplay_tag_name(context);
            }
        }

        // Always push a frame for a matching outer event, including rejected
        // ones. ProcessEvent may nest; the matching post callback consumes these
        // frames in LIFO order and can never borrow evidence from an earlier
        // button activation.
        PendingNativeSuitSelectionEvents.push_back(
            std::move(observation));
    }


    void global_selection_process_event_trace_post(
        UObject* context,
        UFunction* function,
        void* params)
    {
        (void)params;
        if (!context ||
            !function)
        {
            return;
        }

        if (!(ExpandedEvents::flags(function) & (ExpandedEvents::Selection | ExpandedEvents::Generation))) return;
        SelectionTiming timing{L"selection-post-observer"};
        const auto event_name = function->GetName();
        if (NativeSchema3ManifestModeActive &&
            is_native_suit_button_generation_event_name(event_name) &&
            is_suit_grid_button(context))
        {
            if (NativeSuitSelectionMenuGenerationDepth > 0)
            {
                --NativeSuitSelectionMenuGenerationDepth;
            }

            if (NativeSuitSelectionMenuGenerationDepth == 0)
            {
                constexpr uint64_t menu_settling_ms = 150ULL;
                SuitSlotState.MenuGenerationSettlingDeadlineMs =
                    std::max(
                        SuitSlotState.MenuGenerationSettlingDeadlineMs,
                        GetTickCount64() + menu_settling_ms);
            }
            return;
        }

        if (GlobalSelectionProcessEventTraceInternal ||
            NativeManifestProcessEventInternal ||
            !is_native_suit_selection_event_name(event_name) ||
            !is_suit_grid_button(context))
        {
            return;
        }

        if (PendingNativeSuitSelectionEvents.empty())
        {
            suit_vlog(
                STR("{} Ignored unmatched suit activation: event={} phase=post reason=no-pre-observation\n"),
                ModTag,
                event_name);
            return;
        }

        auto observation =
            std::move(PendingNativeSuitSelectionEvents.back());
        PendingNativeSuitSelectionEvents.pop_back();

        if (observation.Button != context ||
            observation.Function != function ||
            observation.EventName != event_name)
        {
            // A mismatched frame means an unexpected nested dispatch occurred.
            // Drop all evidence rather than risk persisting the wrong button.
            PendingNativeSuitSelectionEvents.clear();
            suit_vlog(
                STR("{} Ignored mismatched suit activation: event={} phase=post reason=pre-post-identity-mismatch\n"),
                ModTag,
                event_name);
            return;
        }

        if (!NativeSchema3ManifestModeActive ||
            observation.MenuGenerationSettling ||
            !observation.LiveMenuSignal ||
            !native_suit_selection_tag_is_readable(
                observation.PawnTag))
        {
            suit_vlog(
                STR("{} Ignored unverified suit activation: event={} phase=pre-to-post live_menu_signal={} menu_settling={} tag_readable={}\n"),
                ModTag,
                event_name,
                observation.LiveMenuSignal ? STR("yes") : STR("no"),
                observation.MenuGenerationSettling ? STR("yes") : STR("no"),
                native_suit_selection_tag_is_readable(
                    observation.PawnTag)
                    ? STR("yes")
                    : STR("no"));
            return;
        }

        auto* suit_menu = observation.SuitMenu.Get();
        if (!is_live_uobject_pointer(context) ||
            !is_suit_grid_button(context) ||
            !suit_menu ||
            !is_live_uobject_pointer(suit_menu) ||
            !suit_menu_contains_button(suit_menu, context))
        {
            suit_vlog(
                STR("{} Ignored accepted suit activation: event={} phase=post reason=button-or-owning-menu-no-longer-live\n"),
                ModTag,
                event_name);
            return;
        }

        StringType accepted_pawn_tag{};
        {
            ScopedNativeSuitSelectionProbeGuards probe_guards{};
            accepted_pawn_tag =
                get_suit_button_gameplay_tag_name(context);
        }

        if (!native_suit_selection_tag_is_readable(
                accepted_pawn_tag) ||
            !tag_identity_equals(
                accepted_pawn_tag,
                observation.PawnTag))
        {
            suit_vlog(
                STR("{} Ignored inconsistent suit activation: button={} event={} phase=post tag_matches={}\n"),
                ModTag,
                context->GetFullName(),
                event_name,
                native_suit_selection_tag_is_readable(
                    accepted_pawn_tag) &&
                        tag_identity_equals(
                            accepted_pawn_tag,
                            observation.PawnTag)
                    ? STR("yes")
                    : STR("no"));
            return;
        }

        queue_pending_native_suit_selection_confirmation(
            suit_menu,
            accepted_pawn_tag,
            event_name);
    }

    // Engine FGameplayTagContainer layout: TArray<FGameplayTag> GameplayTags @0x0,
    // TArray<FGameplayTag> ParentTags @0x10. FGameplayTag == FName (8 bytes here),
    // so a tag container's first 16 bytes are a script TArray of FNames.
    struct RedirectScriptArray
    {
        void* Data;
        int32_t Num;
        int32_t Max;
    };

    struct RawGameplayTagContainer
    {
        RedirectScriptArray GameplayTags{};
        RedirectScriptArray ParentTags{};
    };

    void install_global_selection_process_event_trace()
    {
        if (GlobalSelectionProcessEventTracePreCallbackId !=
                Hook::ERROR_ID ||
            GlobalSelectionProcessEventTracePostCallbackId !=
                Hook::ERROR_ID)
        {
            return;
        }

        reset_pending_native_suit_selection_confirmation();

        // Install the consumer first. If a button event happens while the pair is
        // being registered, an unmatched post is harmless; installing the producer
        // first could strand a TLS frame (or generation depth) forever.
        GlobalSelectionProcessEventTracePostCallbackId =
            Hook::RegisterProcessEventPostCallback(
                [](Hook::TCallbackIterationData<void>&,
                   UObject* context,
                   UFunction* function,
                   void* args)
                {
                    global_selection_process_event_trace_post(
                        context,
                        function,
                        args);
                },
                {false,
                 true,
                 STR("LOTDKExpanded"),
                 STR("NativeSuitSelectionObserverPost")});

        if (GlobalSelectionProcessEventTracePostCallbackId ==
            Hook::ERROR_ID)
        {
            suit_log(
                STR("{} ERROR startup: could not register the native suit selection post-observer\n"),
                ModTag);
            return;
        }

        GlobalSelectionProcessEventTracePreCallbackId =
            Hook::RegisterProcessEventPreCallback(
                [](Hook::TCallbackIterationData<void>&,
                   UObject* context,
                   UFunction* function,
                   void* args)
                {
                    global_selection_process_event_trace_pre(
                        context,
                        function,
                        args);
                },
                {false,
                 true,
                 STR("LOTDKExpanded"),
                 STR("NativeSuitSelectionObserverPre")});

        if (GlobalSelectionProcessEventTracePreCallbackId ==
            Hook::ERROR_ID)
        {
            const auto post_callback_id =
                std::exchange(
                    GlobalSelectionProcessEventTracePostCallbackId,
                    Hook::ERROR_ID);
            Hook::UnregisterCallback(post_callback_id);
            suit_log(
                STR("{} ERROR startup: could not register the native suit selection pre-observer\n"),
                ModTag);
            return;
        }

        suit_vlog(
            STR("{} Native suit selection observer installed phases=pre+post+deferred policy=completed-click+owning-live-menu-membership+authoritative-selected-option+menu-generation-guard\n"),
            ModTag);
    }


    void uninstall_global_selection_process_event_trace()
    {
        // Stop producing frames before removing their consumer. This leaves the
        // post hook available to drain a dispatch that began just before teardown.
        const auto pre_callback_id =
            std::exchange(
                GlobalSelectionProcessEventTracePreCallbackId,
                Hook::ERROR_ID);
        if (pre_callback_id != Hook::ERROR_ID &&
            !Hook::UnregisterCallback(pre_callback_id))
        {
            suit_log(
                STR("{} ERROR shutdown: could not unregister the native suit selection pre-observer\n"),
                ModTag);
        }

        const auto post_callback_id =
            std::exchange(
                GlobalSelectionProcessEventTracePostCallbackId,
                Hook::ERROR_ID);
        if (post_callback_id != Hook::ERROR_ID &&
            !Hook::UnregisterCallback(post_callback_id))
        {
            suit_log(
                STR("{} ERROR shutdown: could not unregister the native suit selection post-observer\n"),
                ModTag);
        }

        PendingNativeSuitSelectionEvents.clear();
        NativeSuitSelectionMenuGenerationDepth = 0;
        reset_pending_native_suit_selection_confirmation();
    }


    bool is_batman_playable_actor(
        UObject* object)
    {
        if (!object ||
            !is_probable_runtime_instance(object))
        {
            return false;
        }

        auto* object_class =
            object->GetClassPrivate();

        const auto class_name =
            object_class
                ? object_class->GetFullName()
                : StringType{};

        const auto full_name =
            object->GetFullName();

        return
            (contains(class_name, STR("BP_Batman_")) ||
             contains(full_name, STR("BP_Batman_"))) &&
            (contains(class_name, STR("_Playable")) ||
             contains(full_name, STR("_Playable"))) &&
            !contains(class_name, STR("Widget")) &&
            !contains(full_name, STR("Widget"));
    }

}

class LOTDKExpanded : public CppUserModBase
{
    std::unique_ptr<CppUserModBase> CharacterModes{CreateExpandedCharacterModes()};
public:

    LOTDKExpanded()
    {
        ModName = STR("LOTDKExpanded");
        ModVersion = STR("1.0.0-sep29-compatibility");
        ModDescription = STR("LOTDK Expanded native content framework for LEGO Batman: Legacy of the Dark Knight.");
        ModAuthors = STR("Loomirr");
    }

    ~LOTDKExpanded() override
    {
        CharacterModes.reset();
        uninstall_native_manifest_lazy_scope_callbacks();
        uninstall_native_manifest_process_event_callback();
        uninstall_global_selection_process_event_trace();
    }

private:
    Hook::GlobalCallbackId NativeManifestProcessEventCallbackId{Hook::ERROR_ID};
    Hook::GlobalCallbackId NativeManifestLazyScopeProcessEventCallbackId{Hook::ERROR_ID};
    Hook::GlobalCallbackId NativeManifestConstructDepthPreCallbackId{Hook::ERROR_ID};
    Hook::GlobalCallbackId NativeManifestCinematicLoaderConstructCallbackId{Hook::ERROR_ID};
    std::atomic_bool NativeManifestConstructObserverPairActive{false};

    struct NativeManifestPendingCinematicLoader
    {
        UObject* Loader{};
        uint64_t QueuedAtMs{};
        uint64_t NextPollMs{};
        int32_t PollCount{};
    };

    struct NativeManifestLazyAssetState
    {
        bool Complete{};
        int32_t Attempts{};
        uint64_t NextAttemptMs{};
    };

    std::mutex NativeManifestPendingCinematicLoadersMutex{};
    std::deque<NativeManifestPendingCinematicLoader>
        NativeManifestPendingCinematicLoaders{};
    std::atomic_bool NativeManifestHasPendingCinematicLoaders{false};
    std::atomic_uint64_t NativeManifestPendingCinematicLoadersNextPollMs{0};
    std::unordered_map<StringType, NativeManifestLazyAssetState>
        NativeManifestLazyAssetStates{};

    bool object_is_nested_under(
        UObject* object,
        UObject* expected_outer)
    {
        if (!object || !expected_outer)
        {
            return false;
        }

        auto* current = object->GetOuterPrivate();

        for (int32_t depth = 0;
             current && depth < 64;
             ++depth)
        {
            if (current == expected_outer)
            {
                return true;
            }

            current = current->GetOuterPrivate();
        }

        return false;
    }


    bool is_live_uobject_pointer(
        UObject* candidate)
    {
        if (!candidate)
        {
            return false;
        }

        bool found = false;

        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                if (object == candidate)
                {
                    found = true;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        return found;
    }


    UObject* find_player_controlled_pawn_unfiltered()
    {
        auto* player_controller =
            SuitSlotState.PlayerController;

        // rf258: frontend/world travel can destroy the cached UI virtual player
        // controller before the gameplay controller exists. Never reflect into a
        // cached UObject unless it is still present in GUObjectArray. The rf257
        // crash dump resolved to GetFunctionByNameInChain("GetPawn") on exactly
        // this stale cached controller during the Frontend -> Gotham transition.
        if (player_controller &&
            !is_live_uobject_pointer(player_controller))
        {
            SuitSlotState.PlayerController = nullptr;
            player_controller = nullptr;
        }

        if (!player_controller)
        {
            auto* suit_menu =
                SuitSlotState.SuitMenu;

            if (suit_menu &&
                !is_live_uobject_pointer(suit_menu))
            {
                SuitSlotState.SuitMenu = nullptr;
                suit_menu = nullptr;
            }

            if (suit_menu)
            {
                auto* owning_player =
                    get_widget_owning_player(
                        suit_menu);

                if (owning_player &&
                    is_live_uobject_pointer(owning_player))
                {
                    player_controller = owning_player;
                }
            }

            if (!player_controller)
            {
                auto* discovered_controller =
                    find_first_player_controller();

                if (discovered_controller &&
                    is_live_uobject_pointer(discovered_controller))
                {
                    player_controller = discovered_controller;
                }
            }

            SuitSlotState.PlayerController =
                player_controller;
        }

        if (!player_controller ||
            !is_live_uobject_pointer(player_controller))
        {
            SuitSlotState.PlayerController = nullptr;
            return nullptr;
        }

        auto* pawn =
            call_function_object_return_no_params(
                player_controller,
                STR("GetPawn"));

        if (!pawn)
        {
            pawn =
                get_object_property_value(
                    player_controller,
                    STR("Pawn"));
        }

        return pawn;
    }


    UObject* find_nested_runtime_object_by_class_term(
        UObject* owner,
        StringViewType class_term)
    {
        if (!owner ||
            class_term.empty())
        {
            return nullptr;
        }

        UObject* result = nullptr;

        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                if (!object ||
                    result ||
                    !is_probable_runtime_instance(object) ||
                    !object_is_nested_under(object, owner))
                {
                    return LoopAction::Continue;
                }

                auto* object_class =
                    object->GetClassPrivate();

                const auto class_name =
                    object_class
                        ? object_class->GetFullName()
                        : StringType{};

                if (contains(class_name, class_term))
                {
                    result = object;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        return result;
    }


    UObject* find_runtime_object_by_class_term_with_function(
        StringViewType class_term,
        const wchar_t* function_name)
    {
        if (class_term.empty() ||
            !function_name)
        {
            return nullptr;
        }

        UObject* result = nullptr;

        UObjectGlobals::ForEachUObject(
            [&](UObject* object, ...) -> LoopAction
            {
                if (!object ||
                    result ||
                    !is_probable_runtime_instance(object))
                {
                    return LoopAction::Continue;
                }

                auto* object_class =
                    object->GetClassPrivate();

                const auto class_name =
                    object_class
                        ? object_class->GetFullName()
                        : StringType{};

                if (contains(class_name, class_term) &&
                    object_has_function(object, function_name))
                {
                    result = object;
                    return LoopAction::Break;
                }

                return LoopAction::Continue;
            });

        return result;
    }


    UObject* find_tt_party_pawn_component_for_pawn(
        UObject* pawn)
    {
        if (!pawn)
        {
            return nullptr;
        }

        return find_nested_runtime_object_by_class_term(
            pawn,
            STR("TtPartyPawnComponent"));
    }


    UObject* find_tt_party_controller_component_for_controller(
        UObject* controller)
    {
        if (!controller)
        {
            return nullptr;
        }

        return find_nested_runtime_object_by_class_term(
            controller,
            STR("TtPartyControllerComponent"));
    }


    UObject* find_tt_party_controller_component_for_member(
        UObject* member)
    {
        if (!member)
        {
            return nullptr;
        }

        return call_function_object_return_no_params(
            member,
            STR("GetPartyControllerComponent"));
    }


    UObject* find_tt_party_system_instance()
    {
        return find_runtime_object_by_class_term_with_function(
            STR("TtPartySystem"),
            STR("TogglePartyPawn"));
    }


    bool import_gameplay_tag_param_value(
        FProperty* property,
        void* params,
        UObject* owner,
        StringViewType tag_text)
    {
        if (!property ||
            !params ||
            tag_text.empty())
        {
            return false;
        }

        const StringType import_text =
            STR("(TagName=\"") +
            StringType{tag_text} +
            STR("\")");

        const bool imported =
            property->ImportText_InContainer(
                import_text.c_str(),
                params,
                owner,
                0,
                nullptr) != nullptr;

        if (imported)
        {
            return true;
        }

        auto* storage =
            property->ContainerPtrToValuePtr<void>(
                params);

        if (!storage)
        {
            return false;
        }

        const FName tag_name{
            StringType{tag_text}.c_str(),
            FNAME_Add};

        // FGameplayTag is a thin wrapper around FName TagName in this title.
        std::memcpy(
            storage,
            &tag_name,
            sizeof(FName));

        return true;
    }


    bool call_tt_party_system_toggle_party_pawn(
        UObject* party_system,
        UObject* pawn_component,
        StringViewType target_tag,
        StringViewType reason,
        bool instant_handover = true)
    {
        auto* function =
            party_system
                ? party_system->GetFunctionByNameInChain(
                      STR("TogglePartyPawn"))
                : nullptr;

        if (!party_system ||
            !pawn_component ||
            !function ||
            target_tag.empty())
        {
            suit_vlog(
                STR("{} SuitRuntime PARTY_TARGET_HANDOFF_TOGGLE_SYSTEM_SKIPPED system={} pawn_component={} target_tag={} reason={} failure=missing-required-object-or-function\n"),
                ModTag,
                party_system
                    ? party_system->GetFullName()
                    : STR("<none>"),
                pawn_component
                    ? pawn_component->GetFullName()
                    : STR("<none>"),
                target_tag.empty()
                    ? STR("<none>")
                    : target_tag,
                reason.empty()
                    ? STR("<none>")
                    : reason);
            return false;
        }

        const auto param_size =
            std::max<int32_t>(
                function->GetPropertiesSize(),
                function->GetStructureSize());

        if (param_size <= 0 ||
            param_size > 512)
        {
            return false;
        }

        std::vector<uint8_t> params(
            static_cast<size_t>(param_size),
            0);

        function->InitializeStruct(
            params.data());

        bool pawn_set = false;
        bool tag_set = false;
        bool instant_set = false;

        for (auto* property :
             TFieldRange<FProperty>(
                 function,
                 EFieldIterationFlags::IncludeDeprecated))
        {
            if (!property ||
                !property->HasAnyPropertyFlags(CPF_Parm) ||
                property->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }

            const auto property_name =
                property->GetName();

            // rf260: PawnTagToToggleTo also contains the word "Pawn". The old
            // ordering classified that gameplay-tag struct as the pawn object
            // parameter, failed the object-property cast, then continued before
            // the tag writer could see it. Handle tag parameters first.
            if (property_name == STR("PawnTagToToggleTo") ||
                property_name == STR("GameplayTag") ||
                contains(property_name, STR("Tag")))
            {
                tag_set =
                    import_gameplay_tag_param_value(
                        property,
                        params.data(),
                        party_system,
                        target_tag) ||
                    tag_set;
                continue;
            }

            if (property_name == STR("PawnToToggle") ||
                contains(property_name, STR("Pawn")))
            {
                if (CastField<FObjectPropertyBase>(property))
                {
                    auto* storage =
                        property->ContainerPtrToValuePtr<void>(
                            params.data());

                    if (storage)
                    {
                        *reinterpret_cast<UObject**>(storage) =
                            pawn_component;
                        pawn_set = true;
                    }
                }
                continue;
            }

            if (auto* bool_property =
                    CastField<FBoolProperty>(property))
            {
                if (property_name == STR("bInstantHandover") ||
                    contains(property_name, STR("Instant")))
                {
                    bool_property->SetPropertyValue(
                        bool_property->ContainerPtrToValuePtr<void>(
                            params.data()),
                        instant_handover);
                    instant_set = true;
                }
            }
        }

        const bool previous_internal =
            GlobalSelectionProcessEventTraceInternal;

        GlobalSelectionProcessEventTraceInternal = true;

        party_system->ProcessEvent(
            function,
            params.data());

        GlobalSelectionProcessEventTraceInternal =
            previous_internal;

        function->DestroyStruct(
            params.data());

        suit_vlog(
            STR("{} SuitRuntime PARTY_TARGET_HANDOFF_TOGGLE_SYSTEM_CALLED system={} pawn_component={} target_tag={} pawn_set={} tag_set={} instant_set={} instant_value={} reason={} policy=tt-party-system-toggle-party-pawn-rf270-explicit-handover-mode\n"),
            ModTag,
            party_system->GetFullName(),
            pawn_component->GetFullName(),
            target_tag,
            pawn_set ? STR("yes") : STR("no"),
            tag_set ? STR("yes") : STR("no"),
            instant_set ? STR("yes") : STR("no"),
            instant_handover ? STR("true") : STR("false"),
            reason.empty()
                ? STR("<none>")
                : reason);

        return pawn_set && tag_set;
    }


    bool call_tt_party_member_bool_no_params_safe(
        UObject* party_member,
        StringViewType function_name,
        bool& out_value,
        StringViewType reason)
    {
        out_value = false;

        auto* function =
            party_member && !function_name.empty()
                ? party_member->GetFunctionByNameInChain(
                      StringType{function_name}.c_str())
                : nullptr;

        if (!party_member ||
            !function ||
            function_name.empty())
        {
            suit_vlog(
                STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_SKIPPED member={} function={} reason={} failure=missing-member-or-function\n"),
                ModTag,
                party_member
                    ? party_member->GetFullName()
                    : STR("<none>"),
                function_name.empty()
                    ? STR("<none>")
                    : function_name,
                reason.empty()
                    ? STR("<none>")
                    : reason);
            return false;
        }

        const auto param_size =
            std::max<int32_t>(
                function->GetPropertiesSize(),
                function->GetStructureSize());

        if (param_size < 0 ||
            param_size > 128)
        {
            suit_vlog(
                STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_SKIPPED member={} function={} param_size={} reason={} failure=bad-param-size\n"),
                ModTag,
                party_member->GetFullName(),
                function_name,
                param_size,
                reason.empty()
                    ? STR("<none>")
                    : reason);
            return false;
        }

        if (param_size == 0x01)
        {
            std::array<uint8_t, 0x01> params{};

            suit_vlog(
                STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_ABOUT_TO_CALL member={} function={} param_size={} reason={} call_shape=exact-sdk-bool-return-0x01-native-flag\n"),
                ModTag,
                party_member->GetFullName(),
                function->GetFullName(),
                param_size,
                reason.empty()
                    ? STR("<none>")
                    : reason);

            const bool previous_internal =
                GlobalSelectionProcessEventTraceInternal;

            GlobalSelectionProcessEventTraceInternal = true;

            const uint32_t previous_function_flags =
                function->GetFunctionFlags();

            function->GetFunctionFlags() =
                previous_function_flags | 0x00000400u;

            party_member->ProcessEvent(
                function,
                params.data());

            function->GetFunctionFlags() =
                previous_function_flags;

            GlobalSelectionProcessEventTraceInternal =
                previous_internal;

            out_value =
                params[0x00] != 0;

            suit_vlog(
                STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_RESULT member={} function={} return_captured={} value={} reason={}\n"),
                ModTag,
                party_member->GetFullName(),
                function->GetFullName(),
                STR("yes"),
                out_value ? STR("true") : STR("false"),
                reason.empty()
                    ? STR("<none>")
                    : reason);

            return true;
        }

        std::vector<uint8_t> params(
            static_cast<size_t>(
                std::max<int32_t>(
                    param_size,
                    1)),
            0);

        function->InitializeStruct(
            params.data());

        FBoolProperty* return_bool_property = nullptr;

        for (auto* property :
             TFieldRange<FProperty>(
                 function,
                 EFieldIterationFlags::IncludeDeprecated))
        {
            if (property &&
                property->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                return_bool_property =
                    CastField<FBoolProperty>(
                        property);
                break;
            }
        }

        if (!return_bool_property)
        {
            function->DestroyStruct(
                params.data());
            suit_vlog(
                STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_SKIPPED member={} function={} reason={} failure=missing-return-bool\n"),
                ModTag,
                party_member->GetFullName(),
                function_name,
                reason.empty()
                    ? STR("<none>")
                    : reason);
            return false;
        }

        suit_vlog(
            STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_ABOUT_TO_CALL member={} function={} param_size={} reason={}\n"),
            ModTag,
            party_member->GetFullName(),
            function->GetFullName(),
            param_size,
            reason.empty()
                ? STR("<none>")
                : reason);

        const bool previous_internal =
            GlobalSelectionProcessEventTraceInternal;

        GlobalSelectionProcessEventTraceInternal = true;

        const uint32_t previous_function_flags =
            function->GetFunctionFlags();

        function->GetFunctionFlags() =
            previous_function_flags | 0x00000400u;

        party_member->ProcessEvent(
            function,
            params.data());

        function->GetFunctionFlags() =
            previous_function_flags;

        GlobalSelectionProcessEventTraceInternal =
            previous_internal;

        auto* return_storage =
            return_bool_property->ContainerPtrToValuePtr<void>(
                params.data());

        if (return_storage)
        {
            out_value =
                return_bool_property->GetPropertyValue(
                    return_storage);
        }

        function->DestroyStruct(
            params.data());

        suit_vlog(
            STR("{} SuitRuntime PARTY_MEMBER_BOOL_CALL_RESULT member={} function={} return_captured={} value={} reason={}\n"),
            ModTag,
            party_member->GetFullName(),
            function->GetFullName(),
            return_storage ? STR("yes") : STR("no"),
            out_value ? STR("true") : STR("false"),
            reason.empty()
                ? STR("<none>")
                : reason);

        return return_storage != nullptr;
    }


    // A cold cutscene can request a saved suit for a non-Batman character long
    // after the frontend has finished.  Keep discovery deliberately narrow:
    // derive only a Pawns.Playable.* value that the game already exposed in a
    // reflected tag, metadata object, or identity manager.  This helper never
    // invents a tag and never changes the request.
    StringType native_manifest_playable_tag_from_text(
        StringViewType exported_text)
    {
        constexpr StringViewType playable_prefix =
            STR("Pawns.Playable.");

        auto tag_name =
            extract_exported_gameplay_tag_name(
                exported_text);
        if (tag_identity_starts_with(tag_name, playable_prefix))
        {
            return tag_name;
        }

        const StringType text{exported_text};
        const auto start = text.find(playable_prefix);
        if (start == StringType::npos)
        {
            return {};
        }

        auto end = start;
        while (end < text.size())
        {
            const auto character = text[end];
            if (!std::iswalnum(character) &&
                character != L'_' &&
                character != L'.')
            {
                break;
            }
            ++end;
        }

        return end > start + playable_prefix.size()
            ? text.substr(start, end - start)
            : StringType{};
    }


    StringType native_manifest_playable_tag_from_object(
        UObject* object)
    {
        if (!object)
        {
            return {};
        }

        constexpr std::array<const wchar_t*, 7> tag_properties{
            STR("PawnTag"),
            STR("IdentityTag"),
            STR("InitialIdentity"),
            STR("InitialIdentityTag"),
            STR("CurrentIdentityTag"),
            STR("TargetIdentityTag"),
            STR("PreviousIdentityTag")};

        for (const auto* property_name : tag_properties)
        {
            const auto pawn_tag =
                native_manifest_playable_tag_from_text(
                    get_property_text(
                        object,
                        property_name));
            if (!pawn_tag.empty())
            {
                return pawn_tag;
            }
        }

        return {};
    }


    StringType native_manifest_playable_tag_from_event(
        UObject* context,
        UFunction* function,
        void* params,
        UObject*& out_target_metadata)
    {
        out_target_metadata = nullptr;

        // RequestIdentity and TogglePartyPawn expose the freshest identity in a
        // parameter.  Prefer it over manager state, which may still describe the
        // actor being replaced.
        if (function && params)
        {
            for (auto* property :
                 TFieldRange<FProperty>(
                     function,
                     EFieldIterationFlags::IncludeDeprecated))
            {
                if (!property ||
                    !property->HasAnyPropertyFlags(CPF_Parm) ||
                    property->HasAnyPropertyFlags(CPF_ReturnParm) ||
                    !contains(property->GetName(), STR("Tag")))
                {
                    continue;
                }

                const auto pawn_tag =
                    native_manifest_playable_tag_from_text(
                        export_property_value_from_container(
                            context,
                            property,
                            params));
                if (!pawn_tag.empty())
                {
                    return pawn_tag;
                }
            }
        }

        // Walk only the small identity-loader ownership chain.  A party member,
        // identity manager, or loader can be the ProcessEvent context depending
        // on which Blueprint/native boundary fired first.
        int32_t depth = 0;
        for (auto* current = context;
             current && depth < 8;
             current = current->GetOuterPrivate(), ++depth)
        {
            UObject* loader = nullptr;
            auto* current_class = current->GetClassPrivate();
            const auto class_name =
                current_class
                    ? current_class->GetFullName()
                    : StringType{};

            if (contains(
                    class_name,
                    STR("TtPawnMetaDataIdentityLoader")))
            {
                loader = current;
            }
            else
            {
                loader =
                    get_object_property_value(
                        current,
                        STR("TargetIdentityLoader"));
                if (!loader)
                {
                    auto* identity_manager =
                        get_object_property_value(
                            current,
                            STR("IdentityManager"));
                    loader =
                        get_object_property_value(
                            identity_manager,
                            STR("TargetIdentityLoader"));
                }
            }

            if (loader)
            {
                auto* target_metadata =
                    get_object_property_value(
                        loader,
                        STR("TargetMetaData"));
                if (target_metadata)
                {
                    out_target_metadata = target_metadata;
                    const auto pawn_tag =
                        native_manifest_playable_tag_from_object(
                            target_metadata);
                    if (!pawn_tag.empty())
                    {
                        return pawn_tag;
                    }
                }

                const auto loader_tag =
                    native_manifest_playable_tag_from_object(
                        loader);
                if (!loader_tag.empty())
                {
                    return loader_tag;
                }
            }

            auto* direct_target_metadata =
                get_object_property_value(
                    current,
                    STR("TargetMetaData"));
            if (direct_target_metadata)
            {
                out_target_metadata = direct_target_metadata;
                const auto pawn_tag =
                    native_manifest_playable_tag_from_object(
                        direct_target_metadata);
                if (!pawn_tag.empty())
                {
                    return pawn_tag;
                }
            }

            const auto direct_tag =
                native_manifest_playable_tag_from_object(
                    current);
            if (!direct_tag.empty())
            {
                return direct_tag;
            }

        }

        return {};
    }


    bool ensure_native_manifest_saved_scope_assets_resident(
        const NativeManifestSavedSelection& selection,
        UObject* observed_target_metadata,
        StringViewType trigger)
    {
        constexpr uint64_t retry_interval_ms = 250ULL;
        constexpr uint64_t exhausted_retry_interval_ms = 30000ULL;
        constexpr int32_t max_attempts_per_burst = 8;

        if (!NativeSchema3ManifestModeActive ||
            selection.CharacterScope.empty() ||
            selection.PawnTag.empty())
        {
            return false;
        }

        auto& state =
            NativeManifestLazyAssetStates[selection.PawnTag];
        if (state.Complete)
        {
            return true;
        }

        const uint64_t now = GetTickCount64();
        if (now < state.NextAttemptMs)
        {
            return false;
        }

        const NativeSuitModManifest* manifest = nullptr;
        const auto* manifest_suit =
            find_enabled_native_manifest_suit_by_pawn_tag(
                selection.PawnTag,
                manifest);
        if (!manifest ||
            !manifest_suit ||
            !tag_identity_equals(
                native_manifest_character_scope_for_suit(
                    *manifest_suit),
                selection.CharacterScope))
        {
            state.NextAttemptMs =
                now + exhausted_retry_interval_ms;
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_LAZY_SCOPE_PRELOAD_SKIPPED scope={} pawn_tag={} trigger={} reason=manifest-selection-mismatch retry_ms={} mutation=no\n"),
                ModTag,
                selection.CharacterScope,
                selection.PawnTag,
                trigger.empty() ? STR("<none>") : trigger,
                exhausted_retry_interval_ms);
            return false;
        }

        ++state.Attempts;

        // Blocking asset loads invoke reflected functions internally.  Suppress
        // both native callbacks until this narrow load/root operation unwinds so
        // the same request cannot recursively enter the lazy preloader.
        struct RestoreProcessEventGuards
        {
            bool& NativeGuard;
            bool& SelectionGuard;
            bool PreviousNativeGuard;
            bool PreviousSelectionGuard;

            ~RestoreProcessEventGuards()
            {
                NativeGuard = PreviousNativeGuard;
                SelectionGuard = PreviousSelectionGuard;
            }
        } restore_guards{
            NativeManifestProcessEventInternal,
            GlobalSelectionProcessEventTraceInternal,
            NativeManifestProcessEventInternal,
            GlobalSelectionProcessEventTraceInternal};

        NativeManifestProcessEventInternal = true;
        GlobalSelectionProcessEventTraceInternal = true;

        const auto load_and_root =
            [&](StringViewType object_path,
                bool is_class,
                StringViewType label,
                bool& out_load_called,
                bool& out_rooted_before,
                UObject*& out_object) -> bool
            {
                out_load_called = false;
                out_rooted_before = false;
                out_object =
                    object_path.empty()
                        ? nullptr
                        : find_loaded_object(
                              StringType{object_path}.c_str());

                if (!out_object ||
                    !is_live_uobject_pointer(out_object))
                {
                    out_load_called = true;
                    const StringType owned_path{object_path};
                    out_object =
                        is_class
                            ? kismet_load_class_asset_blocking(
                                  owned_path.c_str(),
                                  label)
                            : kismet_load_asset_blocking(
                                  owned_path.c_str(),
                                  label);

                    auto* found_after =
                        object_path.empty()
                            ? nullptr
                            : find_loaded_object(
                                  owned_path.c_str());
                    if (found_after &&
                        is_live_uobject_pointer(found_after))
                    {
                        out_object = found_after;
                    }
                }

                if (!out_object ||
                    !is_live_uobject_pointer(out_object))
                {
                    return false;
                }

                out_rooted_before = out_object->IsRootSet();
                if (!out_rooted_before)
                {
                    out_object->SetRootSet();
                }
                return out_object->IsRootSet();
            };

        bool dcmd_load_called = false;
        bool dcmd_rooted_before = false;
        UObject* dcmd_object = nullptr;
        const bool dcmd_ready =
            load_and_root(
                manifest_suit->DcmdObjectPath,
                false,
                STR("lazy-saved-character-dcmd-preload"),
                dcmd_load_called,
                dcmd_rooted_before,
                dcmd_object);

        bool playable_load_called = false;
        bool playable_rooted_before = false;
        UObject* playable_class = nullptr;
        const bool playable_ready =
            load_and_root(
                manifest_suit->PlayableClassPath,
                true,
                STR("lazy-saved-character-playable-class-preload"),
                playable_load_called,
                playable_rooted_before,
                playable_class);

        bool cutscene_load_called = false;
        bool cutscene_rooted_before = false;
        UObject* cutscene_class = nullptr;
        const bool cutscene_ready =
            load_and_root(
                manifest_suit->CutsceneClassPath,
                true,
                STR("lazy-saved-character-cutscene-class-preload"),
                cutscene_load_called,
                cutscene_rooted_before,
                cutscene_class);

        const bool observed_metadata_exact =
            observed_target_metadata &&
            dcmd_object &&
            observed_target_metadata == dcmd_object;
        const bool complete =
            dcmd_ready &&
            playable_ready &&
            cutscene_ready;

        state.Complete = complete;
        if (complete)
        {
            state.NextAttemptMs = 0;
        }
        else if (state.Attempts >= max_attempts_per_burst)
        {
            state.Attempts = 0;
            state.NextAttemptMs =
                now + exhausted_retry_interval_ms;
        }
        else
        {
            state.NextAttemptMs =
                now + retry_interval_ms;
        }

        suit_vlog(
            STR("{} SuitRuntime NATIVE_MANIFEST_LAZY_SCOPE_PRELOAD scope={} pawn_tag={} trigger={} attempt={} dcmd_ready={} dcmd_load_called={} dcmd_rooted_before={} playable_ready={} playable_load_called={} playable_rooted_before={} cutscene_ready={} cutscene_load_called={} cutscene_rooted_before={} observed_metadata={} observed_metadata_exact={} mutation=exact-saved-scope-assets-rooted-only tag_write=no identity_request=no spawn=no apply=no possess=no save_write=no result={}\n"),
            ModTag,
            selection.CharacterScope,
            selection.PawnTag,
            trigger.empty() ? STR("<none>") : trigger,
            state.Attempts,
            dcmd_ready ? STR("yes") : STR("no"),
            dcmd_load_called ? STR("yes") : STR("no"),
            dcmd_rooted_before ? STR("yes") : STR("no"),
            playable_ready ? STR("yes") : STR("no"),
            playable_load_called ? STR("yes") : STR("no"),
            playable_rooted_before ? STR("yes") : STR("no"),
            cutscene_ready ? STR("yes") : STR("no"),
            cutscene_load_called ? STR("yes") : STR("no"),
            cutscene_rooted_before ? STR("yes") : STR("no"),
            observed_target_metadata &&
                    is_live_uobject_pointer(observed_target_metadata)
                ? observed_target_metadata->GetFullName()
                : STR("<none-or-stale>"),
            observed_metadata_exact ? STR("yes") : STR("no"),
            complete
                ? STR("saved-character-assets-resident")
                : state.NextAttemptMs > now + retry_interval_ms
                    ? STR("retry-burst-exhausted")
                    : STR("retry-armed"));

        if (complete)
        {
            suit_log(
                STR("{} Prepared saved suit for {}\n"),
                ModTag,
                selection.CharacterScope);
        }

        return complete;
    }


    bool try_prepare_saved_scope_for_observed_tag(
        StringViewType observed_pawn_tag,
        UObject* observed_target_metadata,
        StringViewType trigger)
    {
        const auto character_scope =
            derive_native_manifest_character_scope(
                observed_pawn_tag);
        if (character_scope.empty())
        {
            return false;
        }

        const auto* saved_selection =
            find_saved_native_manifest_selection_for_scope(
                character_scope,
                SavedNativeManifestSelections);
        if (!saved_selection ||
            !saved_selection->ManifestResolved)
        {
            return false;
        }

        const auto selection = *saved_selection;
        return ensure_native_manifest_saved_scope_assets_resident(
            selection,
            observed_target_metadata,
            trigger);
    }


    bool native_manifest_lazy_scope_candidate_event(
        StringViewType event_name)
    {
        return event_name == STR("RequestIdentity") ||
            event_name == STR("TogglePartyPawn") ||
            event_name == STR("RequestLoadIdentity") ||
            event_name == STR("SpawnIdentityIfNeeded") ||
            event_name == STR("GetTargetIdentityActorClass") ||
            event_name == STR("IsTargetLoaderReady") ||
            event_name == STR("TryApplyTargetIdentity") ||
            event_name == STR("TryApplyIdentity") ||
            event_name == STR("ApplyIdentityWhenReady");
    }


    void observe_native_manifest_lazy_scope_event(
        UObject* context,
        UFunction* function,
        void* params)
    {
        if (!NativeSchema3ManifestModeActive ||
            SavedNativeManifestSelections.empty() ||
            !context ||
            !function)
        {
            return;
        }

        if (!(ExpandedEvents::flags(function) & ExpandedEvents::LazyScope)) return;
        const auto event_name = function->GetName();
        if (!native_manifest_lazy_scope_candidate_event(event_name))
        {
            return;
        }

        UObject* target_metadata = nullptr;
        const auto observed_pawn_tag =
            native_manifest_playable_tag_from_event(
                context,
                function,
                params,
                target_metadata);
        if (observed_pawn_tag.empty())
        {
            return;
        }

        try_prepare_saved_scope_for_observed_tag(
            observed_pawn_tag,
            target_metadata,
            event_name);
    }


    void queue_native_manifest_cinematic_loader(
        UObject* loader)
    {
        if (!loader)
        {
            return;
        }

        std::scoped_lock lock{
            NativeManifestPendingCinematicLoadersMutex};

        const auto duplicate =
            std::find_if(
                NativeManifestPendingCinematicLoaders.begin(),
                NativeManifestPendingCinematicLoaders.end(),
                [&](const NativeManifestPendingCinematicLoader& pending)
                {
                    return pending.Loader == loader;
                });
        if (duplicate !=
            NativeManifestPendingCinematicLoaders.end())
        {
            return;
        }

        // This queue contains raw observed UObject pointers and is therefore
        // intentionally short-lived and bounded.  The game-thread consumer
        // verifies every pointer against the live UObject array before access.
        constexpr size_t max_pending_loaders = 64;
        if (NativeManifestPendingCinematicLoaders.size() >=
            max_pending_loaders)
        {
            NativeManifestPendingCinematicLoaders.pop_front();
        }

        NativeManifestPendingCinematicLoaders.push_back(
            NativeManifestPendingCinematicLoader{
                loader,
                GetTickCount64(),
                0,
                0});
        NativeManifestHasPendingCinematicLoaders.store(
            true,
            std::memory_order_release);
    }


    void tick_native_manifest_pending_cinematic_loaders()
    {
        if (!NativeManifestHasPendingCinematicLoaders.load(
                std::memory_order_acquire))
        {
            return;
        }

        const uint64_t now = GetTickCount64();
        const auto next_poll =
            NativeManifestPendingCinematicLoadersNextPollMs.load(
                std::memory_order_acquire);
        if (now < next_poll)
        {
            return;
        }
        NativeManifestPendingCinematicLoadersNextPollMs.store(
            now + 25ULL,
            std::memory_order_release);

        std::deque<NativeManifestPendingCinematicLoader> pending{};
        {
            std::scoped_lock lock{
                NativeManifestPendingCinematicLoadersMutex};
            pending.swap(
                NativeManifestPendingCinematicLoaders);
            NativeManifestHasPendingCinematicLoaders.store(
                false,
                std::memory_order_release);
        }

        constexpr uint64_t loader_timeout_ms = 30000ULL;
        constexpr uint64_t loader_poll_interval_ms = 25ULL;
        constexpr int32_t max_loader_polls = 1200;
        std::deque<NativeManifestPendingCinematicLoader> retry{};

        for (auto& entry : pending)
        {
            if (!entry.Loader ||
                now - entry.QueuedAtMs > loader_timeout_ms ||
                entry.PollCount >= max_loader_polls ||
                !is_live_uobject_pointer(entry.Loader))
            {
                continue;
            }

            if (now < entry.NextPollMs)
            {
                retry.push_back(entry);
                continue;
            }

            ++entry.PollCount;
            entry.NextPollMs =
                now + loader_poll_interval_ms;

            UObject* target_metadata = nullptr;
            const auto observed_pawn_tag =
                native_manifest_playable_tag_from_event(
                    entry.Loader,
                    nullptr,
                    nullptr,
                    target_metadata);
            if (observed_pawn_tag.empty())
            {
                retry.push_back(entry);
                continue;
            }

            const auto character_scope =
                derive_native_manifest_character_scope(
                    observed_pawn_tag);
            const auto* saved_selection =
                find_saved_native_manifest_selection_for_scope(
                    character_scope,
                    SavedNativeManifestSelections);
            if (!saved_selection ||
                !saved_selection->ManifestResolved)
            {
                // This character has no saved custom suit.  Do not retain or
                // preload anything for an unrelated cinematic loader. Dormant
                // rows stay on disk but are never treated as loadable assets.
                continue;
            }

            if (!try_prepare_saved_scope_for_observed_tag(
                    observed_pawn_tag,
                    target_metadata,
                    STR("cinematic-loader-observed")))
            {
                retry.push_back(entry);
            }
        }

        {
            std::scoped_lock lock{
                NativeManifestPendingCinematicLoadersMutex};
            constexpr size_t max_pending_loaders = 64;
            for (auto& entry : retry)
            {
                if (NativeManifestPendingCinematicLoaders.size() >=
                    max_pending_loaders)
                {
                    break;
                }
                NativeManifestPendingCinematicLoaders.push_back(
                    entry);
            }
            NativeManifestHasPendingCinematicLoaders.store(
                !NativeManifestPendingCinematicLoaders.empty(),
                std::memory_order_release);
            if (NativeManifestPendingCinematicLoaders.empty())
            {
                NativeManifestPendingCinematicLoadersNextPollMs.store(
                    0,
                    std::memory_order_release);
            }
        }
    }


    void install_native_manifest_lazy_scope_callbacks()
    {
        if (NativeManifestLazyScopeProcessEventCallbackId == Hook::ERROR_ID)
        {
            NativeManifestLazyScopeProcessEventCallbackId =
                Hook::RegisterProcessEventPreCallback(
                    [this](Hook::TCallbackIterationData<void>&,
                           UObject* context,
                           UFunction* function,
                           void* params)
                    {
                        if (NativeManifestProcessEventInternal ||
                            GlobalSelectionProcessEventTraceInternal ||
                            NativeManifestStaticConstructObjectDepth != 0 ||
                            !NativeSchema3ManifestModeActive ||
                            SavedNativeManifestSelections.empty())
                        {
                            return;
                        }

                        // Construction hooks only enqueue.  Performing the
                        // blocking loads here guarantees the StaticConstructObject
                        // call stack has unwound and we are on a game-thread
                        // ProcessEvent boundary.
                        tick_native_manifest_pending_cinematic_loaders();
                        observe_native_manifest_lazy_scope_event(
                            context,
                            function,
                            params);
                    },
                    {false,
                     true,
                     STR("LOTDKExpanded"),
                     STR("NativeManifestLazyCharacterScope")});

            if (NativeManifestLazyScopeProcessEventCallbackId ==
                Hook::ERROR_ID)
            {
                suit_log(
                    STR("{} ERROR startup: could not register lazy saved-character ProcessEvent callback; non-Batman cold-cutscene preload disabled\n"),
                    ModTag);
            }
        }

        if (NativeManifestLazyScopeProcessEventCallbackId !=
                Hook::ERROR_ID &&
            NativeManifestConstructDepthPreCallbackId ==
                Hook::ERROR_ID &&
            NativeManifestCinematicLoaderConstructCallbackId ==
                Hook::ERROR_ID)
        {
            // As with the ProcessEvent pair, install the consumer before the
            // producer. An unmatched POST during this window is harmless;
            // installing PRE first could strand per-thread construction depth.
            NativeManifestCinematicLoaderConstructCallbackId =
                Hook::RegisterStaticConstructObjectPostCallback(
                    [this](Hook::TCallbackIterationData<UObject*>& callback_data,
                           const FStaticConstructObjectParameters& parameters)
                    {
                        const bool matched_construct_pre =
                            NativeManifestStaticConstructObjectDepth > 0;
                        struct RestoreConstructionDepth
                        {
                            ~RestoreConstructionDepth()
                            {
                                if (NativeManifestStaticConstructObjectDepth > 0)
                                {
                                    --NativeManifestStaticConstructObjectDepth;
                                }
                            }
                        } restore_construction_depth{};

                        // Always balance a PRE that began while the pair was
                        // active. Once teardown starts, skip new observations but
                        // leave the POST consumer available to drain in-flight
                        // calls.
                        if (!NativeManifestConstructObserverPairActive.load(
                                std::memory_order_acquire) ||
                            !matched_construct_pre ||
                            NativeManifestProcessEventInternal)
                        {
                            return;
                        }

                        auto* requested_class =
                            const_cast<UClass*>(parameters.Class);
                        if (!requested_class ||
                            !contains(
                                requested_class->GetFullName(),
                                STR("TtPawnMetaDataIdentityLoader_Cinematic")))
                        {
                            return;
                        }

                        auto* object =
                            callback_data.GetCurrentResolvedReturnValue();
                        if (!object ||
                            contains(
                                object->GetFullName(),
                                STR("Default__")))
                        {
                            return;
                        }

                        // Never load or query the asset registry from inside
                        // StaticConstructObject. Store only the observation and
                        // let the ProcessEvent callback handle it after the
                        // construction stack has returned.
                        queue_native_manifest_cinematic_loader(object);
                    },
                    {false,
                     true,
                     STR("LOTDKExpanded"),
                     STR("NativeManifestCinematicLoaderObservation")});

            if (NativeManifestCinematicLoaderConstructCallbackId ==
                Hook::ERROR_ID)
            {
                suit_log(
                    STR("{} ERROR startup: could not register cinematic-loader observation callback; event-tag preload remains available\n"),
                    ModTag);
                return;
            }

            NativeManifestConstructDepthPreCallbackId =
                Hook::RegisterStaticConstructObjectPreCallback(
                    [this](Hook::TCallbackIterationData<UObject*>&,
                           const FStaticConstructObjectParameters&)
                    {
                        if (NativeManifestConstructObserverPairActive.load(
                                std::memory_order_acquire))
                        {
                            ++NativeManifestStaticConstructObjectDepth;
                        }
                    },
                    {false,
                     true,
                     STR("LOTDKExpanded"),
                     STR("NativeManifestConstructDepthGuard")});

            if (NativeManifestConstructDepthPreCallbackId ==
                Hook::ERROR_ID)
            {
                const auto post_callback_id =
                    std::exchange(
                        NativeManifestCinematicLoaderConstructCallbackId,
                        Hook::ERROR_ID);
                Hook::UnregisterCallback(post_callback_id);
                suit_log(
                    STR("{} ERROR startup: could not register construction-depth guard; cinematic-loader observation disabled\n"),
                    ModTag);
                return;
            }

            NativeManifestConstructObserverPairActive.store(
                true,
                std::memory_order_release);
        }
    }


    void uninstall_native_manifest_lazy_scope_callbacks()
    {
        const auto unregister =
            [&](Hook::GlobalCallbackId& callback_id,
                StringViewType label)
            {
                if (callback_id == Hook::ERROR_ID)
                {
                    return;
                }

                const auto registered_id =
                    std::exchange(
                        callback_id,
                        Hook::ERROR_ID);
                if (!Hook::UnregisterCallback(registered_id))
                {
                    suit_log(
                        STR("{} ERROR shutdown: could not unregister {} callback\n"),
                        ModTag,
                        label);
                }
            };

        NativeManifestConstructObserverPairActive.store(
            false,
            std::memory_order_release);

        // Stop producing construction-depth frames before removing their POST
        // consumer. The inactive POST still balances any call already in flight.
        unregister(
            NativeManifestConstructDepthPreCallbackId,
            STR("construction-depth guard"));
        unregister(
            NativeManifestCinematicLoaderConstructCallbackId,
            STR("cinematic-loader observation"));
        unregister(
            NativeManifestLazyScopeProcessEventCallbackId,
            STR("lazy saved-character ProcessEvent"));

        {
            std::scoped_lock lock{
                NativeManifestPendingCinematicLoadersMutex};
            NativeManifestPendingCinematicLoaders.clear();
            NativeManifestHasPendingCinematicLoaders.store(
                false,
                std::memory_order_release);
            NativeManifestPendingCinematicLoadersNextPollMs.store(
                0,
                std::memory_order_release);
        }
        NativeManifestLazyAssetStates.clear();
        NativeManifestStaticConstructObjectDepth = 0;
    }


    // Warm every enabled native suit's generated classes before a menu preview
    // first needs them. This is intentionally class lifetime management only:
    // it never requests an identity or creates an actor.
    void arm_native_manifest_preview_class_warmup()
    {
        NativeManifestPreviewClassWarmupQueue.clear();
        NativeManifestPreviewClassWarmupArmed = false;
        NativeManifestPreviewClassWarmupComplete = false;
        NativeManifestPreviewClassWarmupGameplayReady = false;
        NativeManifestPreviewClassWarmupNextPollMs = 0;
        NativeManifestPreviewClassWarmupGameplayReadyMs = 0;
        NativeManifestPreviewClassWarmupIndex = 0;
        NativeManifestPreviewClassWarmupAttemptsForCurrent = 0;
        NativeManifestPreviewClassWarmupReadyCount = 0;
        NativeManifestPreviewClassWarmupFailureCount = 0;

        if (!NativeSchema3ManifestModeActive)
        {
            NativeManifestPreviewClassWarmupComplete = true;
            return;
        }

        for (const auto& manifest : LoadedNativeSuitModManifests)
        {
            if (!manifest.Valid)
            {
                continue;
            }

            for (const auto& suit : manifest.Suits)
            {
                if (!suit.Enabled || !suit.Valid)
                {
                    continue;
                }

                NativeManifestPreviewClassWarmupQueue.push_back(
                    NativeManifestPreviewClassWarmupEntry{
                        suit.PawnTag,
                        suit.PlayableClassPath,
                        suit.CutsceneClassPath});
            }
        }

        if (NativeManifestPreviewClassWarmupQueue.empty())
        {
            NativeManifestPreviewClassWarmupComplete = true;
            return;
        }

        NativeManifestPreviewClassWarmupArmed = true;
        suit_log(
            STR("{} Queued native preview classes: suits={} policy=class-preload-and-root-only\n"),
            ModTag,
            NativeManifestPreviewClassWarmupQueue.size());
    }

    void tick_native_manifest_preview_class_warmup()
    {
        constexpr uint64_t retry_interval_ms = 250ULL;
        constexpr int32_t max_attempts_per_suit = 8;

        if (!NativeManifestPreviewClassWarmupArmed ||
            NativeManifestPreviewClassWarmupComplete)
        {
            return;
        }

        // The normal saved-suit preload proved that generated classes can be
        // loaded safely once the game has settled. Loading a non-selected
        // preview class during engine/frontend construction is not equivalent:
        // Electric's load begins but the process exits before the class becomes
        // resident. A suit menu only exists in gameplay, so wait for a live pawn
        // in a non-frontend level and give that handover a short settle window.
        if (NativeManifestStartupClassPreloadArmed &&
            !NativeManifestStartupClassPreloadComplete)
        {
            return;
        }

        const uint64_t now = GetTickCount64();
        if (!NativeManifestPreviewClassWarmupGameplayReady)
        {
            auto* pawn = find_player_controlled_pawn_unfiltered();
            const bool pawn_live = pawn && is_live_uobject_pointer(pawn);
            const auto gameplay_world =
                pawn_live
                    ? gameplay_world_key_from_object_name(pawn->GetFullName())
                    : StringType{};
            if (!pawn_live || gameplay_world.empty())
            {
                return;
            }

            NativeManifestPreviewClassWarmupGameplayReady = true;
            NativeManifestPreviewClassWarmupGameplayReadyMs = now;
            suit_log(
                STR("{} Native preview warmup waiting for gameplay settle: suits={}\n"),
                ModTag,
                NativeManifestPreviewClassWarmupQueue.size());
            return;
        }

        constexpr uint64_t gameplay_settle_ms = 1000ULL;
        if (now < NativeManifestPreviewClassWarmupGameplayReadyMs +
                      gameplay_settle_ms)
        {
            return;
        }

        if (NativeManifestPreviewClassWarmupIndex >=
            NativeManifestPreviewClassWarmupQueue.size())
        {
            NativeManifestPreviewClassWarmupArmed = false;
            NativeManifestPreviewClassWarmupComplete = true;
            suit_log(
                STR("{} Native preview classes ready: prepared={}/{} failed={}\n"),
                ModTag,
                NativeManifestPreviewClassWarmupReadyCount,
                NativeManifestPreviewClassWarmupQueue.size(),
                NativeManifestPreviewClassWarmupFailureCount);
            return;
        }

        if (now < NativeManifestPreviewClassWarmupNextPollMs)
        {
            return;
        }
        NativeManifestPreviewClassWarmupNextPollMs = now + retry_interval_ms;
        ++NativeManifestPreviewClassWarmupAttemptsForCurrent;

        const auto& entry =
            NativeManifestPreviewClassWarmupQueue[
                NativeManifestPreviewClassWarmupIndex];

        const auto ensure_class_resident =
            [&](StringViewType class_path,
                StringViewType label) -> bool
            {
                UObject* object =
                    class_path.empty()
                        ? nullptr
                        : find_loaded_object(StringType{class_path}.c_str());
                if (!object || !is_live_uobject_pointer(object))
                {
                    const StringType owned_path{class_path};
                    object =
                        owned_path.empty()
                            ? nullptr
                            : kismet_load_class_asset_blocking(
                                  owned_path.c_str(),
                                  label);
                }

                if ((!object || !is_live_uobject_pointer(object)) &&
                    !class_path.empty())
                {
                    object =
                        find_loaded_object(StringType{class_path}.c_str());
                }

                if (!object || !is_live_uobject_pointer(object))
                {
                    return false;
                }

                if (!object->IsRootSet())
                {
                    object->SetRootSet();
                }
                return object->IsRootSet();
            };

        const bool playable_ready =
            ensure_class_resident(
                entry.PlayableClassPath,
                STR("native-preview-playable-class-preload"));
        const bool cutscene_ready =
            ensure_class_resident(
                entry.CutsceneClassPath,
                STR("native-preview-cutscene-class-preload"));
        const bool complete = playable_ready && cutscene_ready;
        const bool attempts_exhausted =
            NativeManifestPreviewClassWarmupAttemptsForCurrent >=
            max_attempts_per_suit;

        if (!complete && !attempts_exhausted)
        {
            return;
        }

        if (complete)
        {
            ++NativeManifestPreviewClassWarmupReadyCount;
        }
        else
        {
            ++NativeManifestPreviewClassWarmupFailureCount;
            suit_log(
                STR("{} ERROR preview preload: pawn_tag={} playable_ready={} cutscene_ready={} attempts={}\n"),
                ModTag,
                entry.PawnTag,
                playable_ready ? STR("yes") : STR("no"),
                cutscene_ready ? STR("yes") : STR("no"),
                NativeManifestPreviewClassWarmupAttemptsForCurrent);
        }

        ++NativeManifestPreviewClassWarmupIndex;
        NativeManifestPreviewClassWarmupAttemptsForCurrent = 0;
        NativeManifestPreviewClassWarmupNextPollMs = now + 50ULL;
    }

    // rf293: the saved custom DCMD can be registered and resolve exactly while
    // its generated Blueprint classes are still absent. rf290/rf292 proved that
    // a cinematic loader in that state reports ready but cannot resolve its
    // actor class until the cutscene BlueprintGeneratedClass is made resident.
    //
    // Preload only the saved manifest suit's playable and cutscene classes at
    // the earliest safe opportunity. This is deliberately independent of an
    // identity manager: it performs no RequestIdentity, spawn, apply, handover,
    // possession, party/save property write, or save write. The game remains
    // solely responsible for consuming the saved tag and constructing both
    // actors in its normal startup sequence.
    //
    // rf298: rf297's transition trace proved both generated classes became
    // resident, then were collected roughly one second later -- well before
    // DA_Party_FrontEnd built its Batman member. Keep these two UClass objects
    // rooted for the process lifetime after the saved-suit-only preload. This
    // is asset lifetime management, not an identity or party-state mutation.
    void tick_native_manifest_startup_class_preload()
    {
        constexpr uint64_t retry_interval_ms = 250ULL;
        constexpr int32_t max_attempts = 8;

        if (!NativeManifestStartupClassPreloadArmed ||
            NativeManifestStartupClassPreloadComplete ||
            !NativeSchema3ManifestModeActive ||
            !SavedNativeManifestSelectionLoaded ||
            SavedNativeManifestPawnTag.empty())
        {
            return;
        }

        const uint64_t now = GetTickCount64();
        if (NativeManifestStartupClassPreloadStartedMs == 0)
        {
            NativeManifestStartupClassPreloadStartedMs = now;
            NativeManifestStartupClassPreloadPawnTag =
                SavedNativeManifestPawnTag;

            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_CLASS_PRELOAD_BEGIN pawn_tag={} mod_id={} suit_id={} policy=saved-suit-only-preload-playable-and-cutscene-classes-before-native-startup-consumption-no-identity-request-no-spawn-no-apply-no-possess-no-save-write max_attempts={}\n"),
                ModTag,
                SavedNativeManifestPawnTag,
                SavedNativeManifestModId.empty()
                    ? STR("<none>")
                    : SavedNativeManifestModId,
                SavedNativeManifestSuitId.empty()
                    ? STR("<none>")
                    : SavedNativeManifestSuitId,
                max_attempts);
        }

        if (!tag_identity_equals(
                NativeManifestStartupClassPreloadPawnTag,
                SavedNativeManifestPawnTag))
        {
            NativeManifestStartupClassPreloadArmed = false;
            NativeManifestStartupClassPreloadComplete = true;
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_CLASS_PRELOAD_END result=cancelled-selection-changed armed_tag={} current_tag={} load_called=no\n"),
                ModTag,
                NativeManifestStartupClassPreloadPawnTag,
                SavedNativeManifestPawnTag);
            return;
        }

        if (now < NativeManifestStartupClassPreloadNextPollMs)
        {
            return;
        }
        NativeManifestStartupClassPreloadNextPollMs =
            now + retry_interval_ms;
        ++NativeManifestStartupClassPreloadPollCount;

        const NativeSuitModManifest* manifest = nullptr;
        const auto* manifest_suit =
            find_enabled_native_manifest_suit_by_pawn_tag(
                SavedNativeManifestPawnTag,
                manifest);
        if (!manifest || !manifest_suit)
        {
            NativeManifestStartupClassPreloadArmed = false;
            NativeManifestStartupClassPreloadComplete = true;
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_CLASS_PRELOAD_END result=manifest-row-disappeared pawn_tag={} load_called=no\n"),
                ModTag,
                SavedNativeManifestPawnTag);
            return;
        }

        if (PreparedSuitClasses.ready(SavedNativeManifestPawnTag,
                manifest_suit->PlayableClassPath, manifest_suit->CutsceneClassPath,
                [](const FWeakObjectPtr& handle) {
                    auto* object = handle.Get();
                    return object && object->IsRootSet();
                }))
        {
            NativeManifestStartupClassPreloadArmed = false;
            NativeManifestStartupClassPreloadComplete = true;
            static unsigned hitReports{};
            if (hitReports++ < 32) suit_log(
                STR("{} Prepared suit reuse: pawn_tag={} preload_ms={} full_object_scans=0 load_calls=0\n"),
                ModTag, SavedNativeManifestPawnTag,
                GetTickCount64() - NativeManifestStartupClassPreloadStartedMs);
            return;
        }

        const auto preload_class =
            [&](StringViewType class_path,
                StringViewType label,
                UObject*& out_before,
                UObject*& out_load_result,
                UObject*& out_after,
                bool& out_load_called) -> bool
            {
                out_before =
                    class_path.empty()
                        ? nullptr
                        : find_loaded_object(
                              StringType{class_path}.c_str());
                const bool before_live =
                    out_before &&
                    is_live_uobject_pointer(out_before);

                out_load_called = false;
                out_load_result = out_before;
                // rf312: these are generated Blueprint class paths carried by
                // TSoftClassPtr properties. Use the engine's class-specific
                // blocking loader in every mode. The earlier null results came
                // from sending this payload to LoadAsset_Blocking, whose input
                // is TSoftObjectPtr<UObject>. The rf310 proof-mode suppression
                // therefore hid the only correct way to make a custom
                // cutscene class resident before frontend party construction.
                if (!before_live &&
                    !class_path.empty())
                {
                    out_load_called = true;
                    NativeManifestStartupClassPreloadAttempted = true;
                    const StringType owned_class_path{class_path};
                    out_load_result =
                        kismet_load_class_asset_blocking(
                            owned_class_path.c_str(),
                            label);
                }

                out_after =
                    class_path.empty()
                        ? nullptr
                        : find_loaded_object(
                              StringType{class_path}.c_str());
                if ((!out_after ||
                     !is_live_uobject_pointer(out_after)) &&
                    out_load_result &&
                    is_live_uobject_pointer(out_load_result))
                {
                    out_after = out_load_result;
                }

                return out_after &&
                       is_live_uobject_pointer(out_after);
            };

        UObject* playable_before = nullptr;
        UObject* playable_load_result = nullptr;
        UObject* playable_after = nullptr;
        bool playable_load_called = false;
        const bool playable_ready =
            preload_class(
                manifest_suit->PlayableClassPath,
                STR("startup-saved-native-playable-class-preload"),
                playable_before,
                playable_load_result,
                playable_after,
                playable_load_called);

        UObject* cutscene_before = nullptr;
        UObject* cutscene_load_result = nullptr;
        UObject* cutscene_after = nullptr;
        bool cutscene_load_called = false;
        const bool cutscene_ready =
            preload_class(
                manifest_suit->CutsceneClassPath,
                STR("startup-saved-native-cutscene-class-preload"),
                cutscene_before,
                cutscene_load_result,
                cutscene_after,
                cutscene_load_called);

        bool playable_rooted_before = false;
        bool playable_rooted_after = false;
        if (playable_ready &&
            playable_after &&
            is_live_uobject_pointer(playable_after))
        {
            playable_rooted_before = playable_after->IsRootSet();
            if (!playable_rooted_before)
            {
                playable_after->SetRootSet();
            }
            playable_rooted_after = playable_after->IsRootSet();
        }

        bool cutscene_rooted_before = false;
        bool cutscene_rooted_after = false;
        if (cutscene_ready &&
            cutscene_after &&
            is_live_uobject_pointer(cutscene_after))
        {
            cutscene_rooted_before = cutscene_after->IsRootSet();
            if (!cutscene_rooted_before)
            {
                cutscene_after->SetRootSet();
            }
            cutscene_rooted_after = cutscene_after->IsRootSet();
        }

        const bool complete =
            playable_ready &&
            cutscene_ready &&
            playable_rooted_after &&
            cutscene_rooted_after;
        const bool attempts_exhausted =
            NativeManifestStartupClassPreloadPollCount >=
            max_attempts;

        suit_vlog(
            STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_CLASS_PRELOAD_ATTEMPT pawn_tag={} attempt={} playable_path={} playable_before={} playable_load_called={} playable_load_result={} playable_after={} playable_ready={} playable_rooted_before={} playable_rooted_after={} cutscene_path={} cutscene_before={} cutscene_load_called={} cutscene_load_result={} cutscene_after={} cutscene_ready={} cutscene_rooted_before={} cutscene_rooted_after={} elapsed_ms={} mutation=saved-generated-class-gc-root-only party_write=no identity_request=no spawn=no apply=no possess=no save_write=no result={}\n"),
            ModTag,
            SavedNativeManifestPawnTag,
            NativeManifestStartupClassPreloadPollCount,
            manifest_suit->PlayableClassPath,
            playable_before && is_live_uobject_pointer(playable_before)
                ? playable_before->GetFullName()
                : STR("<null-or-stale>"),
            playable_load_called ? STR("yes") : STR("no"),
            playable_load_result &&
                    is_live_uobject_pointer(playable_load_result)
                ? playable_load_result->GetFullName()
                : STR("<null-or-stale>"),
            playable_after && is_live_uobject_pointer(playable_after)
                ? playable_after->GetFullName()
                : STR("<null-or-stale>"),
            playable_ready ? STR("yes") : STR("no"),
            playable_rooted_before ? STR("yes") : STR("no"),
            playable_rooted_after ? STR("yes") : STR("no"),
            manifest_suit->CutsceneClassPath,
            cutscene_before && is_live_uobject_pointer(cutscene_before)
                ? cutscene_before->GetFullName()
                : STR("<null-or-stale>"),
            cutscene_load_called ? STR("yes") : STR("no"),
            cutscene_load_result &&
                    is_live_uobject_pointer(cutscene_load_result)
                ? cutscene_load_result->GetFullName()
                : STR("<null-or-stale>"),
            cutscene_after && is_live_uobject_pointer(cutscene_after)
                ? cutscene_after->GetFullName()
                : STR("<null-or-stale>"),
            cutscene_ready ? STR("yes") : STR("no"),
            cutscene_rooted_before ? STR("yes") : STR("no"),
            cutscene_rooted_after ? STR("yes") : STR("no"),
            now - NativeManifestStartupClassPreloadStartedMs,
            complete
                ? STR("both-generated-classes-resident")
                : attempts_exhausted
                    ? STR("attempts-exhausted")
                    : STR("retry-armed"));

        if (!complete && !attempts_exhausted)
        {
            return;
        }

        NativeManifestStartupClassPreloadArmed = false;
        NativeManifestStartupClassPreloadComplete = true;
        suit_vlog(
            STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_CLASS_PRELOAD_END result={} pawn_tag={} attempts={} playable_ready={} playable_rooted={} cutscene_ready={} cutscene_rooted={} elapsed_ms={} policy=saved-generated-classes-rooted-for-process-lifetime-no-party-write-no-identity-request next=do-not-use-Ctrl+Shift+F8-or-Ctrl+Shift+F9-observe-native-frontend-then-load-gameplay\n"),
            ModTag,
            complete
                ? STR("saved-generated-classes-resident-before-native-consumption")
                : STR("class-preload-failed"),
            SavedNativeManifestPawnTag,
            NativeManifestStartupClassPreloadPollCount,
            playable_ready ? STR("yes") : STR("no"),
            playable_rooted_after ? STR("yes") : STR("no"),
            cutscene_ready ? STR("yes") : STR("no"),
            cutscene_rooted_after ? STR("yes") : STR("no"),
            now - NativeManifestStartupClassPreloadStartedMs);


    if (complete)
    {
        PreparedSuitClasses.remember(SavedNativeManifestPawnTag,
            manifest_suit->PlayableClassPath, manifest_suit->CutsceneClassPath,
            FWeakObjectPtr(playable_after), FWeakObjectPtr(cutscene_after));
        suit_log(
            STR("{} Prepared saved suit assets: pawn_tag={} total_preload_ms={}\n"),
            ModTag,
            SavedNativeManifestPawnTag,
            GetTickCount64() - NativeManifestStartupClassPreloadStartedMs);
    }
    else
    {
        suit_log(
            STR("{} ERROR preload: generated classes unavailable after {} attempts pawn_tag={} playable_ready={} cutscene_ready={}\n"),
            ModTag,
            NativeManifestStartupClassPreloadPollCount,
            SavedNativeManifestPawnTag,
            playable_ready ? STR("yes") : STR("no"),
            cutscene_ready ? STR("yes") : STR("no"));
    }
}


    // rf256/rf259/rf303/rf305: restore a schema-3 custom suit through the game-owned party
    // system boundary, not by copying transient preference bytes and
    // not by driving the lower-level identity manager directly. The worker is
    // armed only by reading selected_native_suit.json during startup. Before it
    // mutates anything, the persistent party-preference save must name the same
    // manifest tag and the live metadata system must resolve that tag to the
    // manifest's exact rooted DCMD. CharacterSelectSaveData remains diagnostic:
    // story-party construction can legitimately clear its Batman costume entry
    // while preserving the player's persistent preferred identity. If the
    // transient preference map has
    // already restored to a live identity/pawn naturally, the worker exits
    // without calling TogglePawn. A transient preference-map entry by itself is
    // not success: frontend startup can briefly contain the saved custom tag
    // before gameplay rebuilds that map and spawns a fallback Batman. Otherwise
    // the worker dispatches exactly once and performs only read-only audits.
    void tick_native_manifest_startup_restore()
    {
        constexpr uint64_t poll_interval_ms = 250ULL;
        constexpr uint64_t prerequisite_timeout_ms = 180000ULL;
        constexpr uint64_t post_dispatch_timeout_ms = 10000ULL;
        constexpr uint64_t stable_gameplay_ready_ms = 1000ULL;
        constexpr int32_t stable_gameplay_observations = 4;

        if (!NativeManifestStartupRestoreArmed ||
            NativeManifestStartupRestoreComplete ||
            !NativeSchema3ManifestModeActive ||
            !SavedNativeManifestSelectionLoaded ||
            SavedNativeManifestPawnTag.empty())
        {
            return;
        }

        const uint64_t now = GetTickCount64();
        if (NativeManifestStartupRestoreStartedMs == 0)
        {
            NativeManifestStartupRestoreStartedMs = now;
            NativeManifestStartupRestorePawnTag =
                SavedNativeManifestPawnTag;

            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_ARMED pawn_tag={} mod_id={} suit_id={} policy=one-animated-party-system-toggle-current-gameplay-pawn-after-exact-registration-and-persistent-preference-confirmation-character-selection-diagnostic-only-no-runtime-map-import-no-requestidentity-no-manual-possess-no-save-write\n"),
                ModTag,
                SavedNativeManifestPawnTag,
                SavedNativeManifestModId.empty()
                    ? STR("<none>")
                    : SavedNativeManifestModId,
                SavedNativeManifestSuitId.empty()
                    ? STR("<none>")
                    : SavedNativeManifestSuitId);
        }

        if (!tag_identity_equals(
                NativeManifestStartupRestorePawnTag,
                SavedNativeManifestPawnTag))
        {
            NativeManifestStartupRestoreComplete = true;
            NativeManifestStartupRestoreArmed = false;
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_END result=cancelled-selection-changed armed_tag={} current_tag={} toggle_called=no\n"),
                ModTag,
                NativeManifestStartupRestorePawnTag,
                SavedNativeManifestPawnTag);
            return;
        }

        if (now < NativeManifestStartupRestoreNextPollMs)
        {
            return;
        }
        NativeManifestStartupRestoreNextPollMs =
            now + poll_interval_ms;
        ++NativeManifestStartupRestorePollCount;

        const NativeSuitModManifest* manifest = nullptr;
        const auto* manifest_suit =
            find_enabled_native_manifest_suit_by_pawn_tag(
                SavedNativeManifestPawnTag,
                manifest);
        // rf310: this is a gameplay handover worker. rf309 ran its metadata and
        // generated-class probes during FlowStarter/frontend startup even though
        // no gameplay pawn existed. The Thomas crash ended immediately after
        // rooting its exact DCMD, before the following class probe could log, and
        // the dump faulted inside UE4SS object lookup. Do not touch metadata,
        // generated classes, party members, or save objects until a live pawn in
        // a non-frontend /Game/Levels world exists.
        auto* startup_gameplay_pawn =
            find_player_controlled_pawn_unfiltered();
        const bool startup_gameplay_pawn_live =
            startup_gameplay_pawn &&
            is_live_uobject_pointer(startup_gameplay_pawn);
        const auto startup_gameplay_pawn_name =
            startup_gameplay_pawn_live
                ? startup_gameplay_pawn->GetFullName()
                : StringType{};
        const auto startup_gameplay_world_key =
            startup_gameplay_pawn_live
                ? gameplay_world_key_from_object_name(
                      startup_gameplay_pawn_name)
                : StringType{};
        if (!startup_gameplay_pawn_live ||
            startup_gameplay_world_key.empty())
        {
            if (NativeManifestStartupRestorePollCount == 1 ||
                NativeManifestStartupRestorePollCount % 120 == 0)
            {
                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_WAIT_GAMEPLAY pawn_tag={} poll={} elapsed_ms={} pawn={} gameplay_world={} action=wait-before-metadata-or-class-probe mutation=no\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    NativeManifestStartupRestorePollCount,
                    now - NativeManifestStartupRestoreStartedMs,
                    startup_gameplay_pawn_name.empty()
                        ? STR("<none>")
                        : startup_gameplay_pawn_name,
                    startup_gameplay_world_key.empty()
                        ? STR("<none>")
                        : startup_gameplay_world_key);
            }
            return;
        }
        auto* metadata_system =
            find_live_tt_pawn_metadata_system();
        auto* resolved_metadata =
            call_tt_pawn_metadata_system_get_metadata_for_tag(
                metadata_system,
                SavedNativeManifestPawnTag,
                STR("native-manifest-startup-restore-preflight"));
        auto* exact_metadata =
            manifest_suit
                ? find_loaded_object(
                      manifest_suit->DcmdObjectPath.c_str())
                : nullptr;
        const bool metadata_resolves_exact =
            manifest &&
            manifest_suit &&
            resolved_metadata &&
            exact_metadata &&
            is_live_uobject_pointer(resolved_metadata) &&
            is_live_uobject_pointer(exact_metadata) &&
            resolved_metadata == exact_metadata;
        bool metadata_rooted_before = false;
        bool metadata_rooted_after = false;
        if (metadata_resolves_exact)
        {
            metadata_rooted_before = exact_metadata->IsRootSet();
            if (!metadata_rooted_before)
            {
                // rf303 promotes the exact successful rf301 F9 preflight into
                // this one-shot startup worker. The metadata system resolves the
                // correct row, but that private registry does not retain a
                // process-long strong reference. Root only the verified manifest
                // DCMD before the asynchronous game-owned handover.
                exact_metadata->SetRootSet();
                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_DCMD_ROOT pawn_tag={} metadata={} rooted_before=no rooted_after={} policy=verified-exact-manifest-dcmd-only-process-lifetime-root-no-party-write-no-save-write\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    exact_metadata->GetFullName(),
                    exact_metadata->IsRootSet() ? STR("yes") : STR("no"));
            }
            metadata_rooted_after = exact_metadata->IsRootSet();
        }
        const bool metadata_exact =
            metadata_resolves_exact &&
            metadata_rooted_after;
        auto* playable_class =
            manifest_suit
                ? find_loaded_object(
                      manifest_suit->PlayableClassPath.c_str())
                : nullptr;
        auto* cutscene_class =
            manifest_suit
                ? find_loaded_object(
                      manifest_suit->CutsceneClassPath.c_str())
                : nullptr;
        const bool generated_classes_ready =
            playable_class &&
            cutscene_class &&
            is_live_uobject_pointer(playable_class) &&
            is_live_uobject_pointer(cutscene_class) &&
            playable_class->IsRootSet() &&
            cutscene_class->IsRootSet();

        auto* player_preferences =
            find_first_runtime_object_derived_from(
                STR("/Script/TtPartySystem.TtPartyPlayerPreferences"));
        auto* preferences_save_data =
            player_preferences
                ? get_object_property_value(
                      player_preferences,
                      STR("CurrentSaveData"))
                : nullptr;
        auto* character_select_save_data =
            find_first_runtime_object_derived_from(
                STR("/Script/DinnerCharacterSelect.CharacterSelectSaveData"));

        const auto runtime_preferences =
            player_preferences
                ? get_property_text(
                      player_preferences,
                      STR("MostRecentIdentityByGroup"))
                : StringType{};
        const auto saved_preferences =
            preferences_save_data
                ? get_property_text(
                      preferences_save_data,
                      STR("MostRecentIdentityByGroup"))
                : StringType{};
        const auto character_selection =
            character_select_save_data
                ? get_property_text(
                      character_select_save_data,
                      STR("SelectionSaveData"))
                : StringType{};
        const auto runtime_manifest_snapshots =
            player_preferences
                ? get_property_text(
                      player_preferences,
                      STR("ManifestSnapshots"))
                : StringType{};
        const auto saved_manifest_snapshots =
            preferences_save_data
                ? get_property_text(
                      preferences_save_data,
                      STR("ManifestSnapshots"))
                : StringType{};

        const bool persistent_preferences_confirm_target =
            preferences_save_data &&
            exported_text_contains_tag_identity(
                saved_preferences,
                SavedNativeManifestPawnTag);
        const bool character_selection_confirms_target =
            character_select_save_data &&
            exported_text_contains_tag_identity(
                character_selection,
                SavedNativeManifestPawnTag);
        const bool runtime_preferences_contains_target =
            player_preferences &&
            exported_text_contains_tag_identity(
                runtime_preferences,
                SavedNativeManifestPawnTag);

        auto* player_controller =
            find_first_player_controller();
        auto* controller_component =
            player_controller &&
                    is_live_uobject_pointer(player_controller)
                ? find_tt_party_controller_component_for_controller(
                      player_controller)
                : nullptr;
        auto* pawn =
            startup_gameplay_pawn;
        auto* pawn_component =
            pawn && is_live_uobject_pointer(pawn)
                ? find_tt_party_pawn_component_for_pawn(pawn)
                : nullptr;
        auto* party_system =
            find_tt_party_system_instance();
        auto* party_member =
            controller_component
                ? call_function_object_return_no_params(
                      controller_component,
                      STR("GetPartyMember"))
                : nullptr;
        if (!party_member && pawn_component)
        {
            party_member =
                call_function_object_return_no_params(
                    pawn_component,
                    STR("GetPartyMember"));
        }
        if (!controller_component && party_member)
        {
            controller_component =
                find_tt_party_controller_component_for_member(
                    party_member);
        }

        const bool party_member_live =
            party_member &&
            is_live_uobject_pointer(party_member);
        const bool controller_component_live =
            controller_component &&
            is_live_uobject_pointer(controller_component);
        const bool pawn_component_live =
            pawn_component &&
            is_live_uobject_pointer(pawn_component);
        const bool party_system_live =
            party_system &&
            is_live_uobject_pointer(party_system);
        const auto current_identity =
            party_member_live
                ? call_function_return_text(
                      party_member,
                      STR("GetCurrentIdentity"))
                : StringType{};
        const auto intended_identity =
            party_member_live
                ? call_function_return_text(
                      party_member,
                      STR("GetIntendedIdentity"))
                : StringType{};
        const bool gameplay_candidate_live =
            pawn &&
            is_live_uobject_pointer(pawn) &&
            pawn_component_live &&
            party_member_live &&
            party_system_live;
        if (gameplay_candidate_live &&
            NativeManifestStartupRestoreGameplayObservedMs == 0)
        {
            NativeManifestStartupRestoreGameplayObservedMs = now;
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_GAMEPLAY_CANDIDATE pawn_tag={} pawn={} pawn_component={} party_member={} current_identity={} action=begin-stability-window minimum_ready_ms={} minimum_observations={}\n"),
                ModTag,
                SavedNativeManifestPawnTag,
                pawn->GetFullName(),
                pawn_component->GetFullName(),
                party_member->GetFullName(),
                current_identity.empty()
                    ? STR("<none>")
                    : truncate_registry_probe_text(
                          current_identity,
                          500),
                stable_gameplay_ready_ms,
                stable_gameplay_observations);
        }
        const bool current_identity_is_target =
            exported_text_contains_tag_identity(
                current_identity,
                SavedNativeManifestPawnTag);
        const auto pawn_class_name =
            pawn &&
                    is_live_uobject_pointer(pawn) &&
                    pawn->GetClassPrivate()
                ? pawn->GetClassPrivate()->GetFullName()
                : StringType{};
        const bool pawn_class_is_target =
            manifest_suit &&
            !pawn_class_name.empty() &&
            contains(
                pawn_class_name,
                manifest_suit->PlayablePackage);

        // rf257: do not accept the transient preference map (or merely intended
        // identity) as proof of a completed restore. rf256 showed that the map
        // can contain Electric while no party member/pawn exists, then be
        // replaced with DarkKnight when Gotham creates the gameplay party. Only
        // a live target pawn, or a live party member that reports the target as
        // its current (not merely intended) identity, is observable completion.
        // The latter remains valid when controller possession is live but the
        // generic controlled-pawn finder cannot see that gameplay pawn.
        // rf303: a correctly restored frontend cinematic identity is not proof
        // that Gotham's story party accepted the saved gameplay identity. Only
        // complete this worker after a real controlled gameplay pawn/component
        // exists; otherwise the Electric frontend would disarm the worker before
        // DarkKnight is created.
        const bool native_restore_observed =
            gameplay_candidate_live &&
            (pawn_class_is_target ||
             current_identity_is_target);

        if (native_restore_observed)
        {
            NativeManifestStartupRestoreComplete = true;
            NativeManifestStartupRestoreArmed = false;
            suit_log(
                STR("{} Restored custom suit: pawn_tag={} method={}\n"),
                ModTag,
                SavedNativeManifestPawnTag,
                NativeManifestStartupRestoreDispatched
                    ? STR("guarded-party-handover")
                    : STR("native-startup"));
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_END result=native-target-observed pawn_tag={} dispatched={} runtime_preferences_match={} current_identity={} intended_identity={} pawn={} pawn_class={} evidence={} elapsed_ms={} policy=no-further-toggle\n"),
                ModTag,
                SavedNativeManifestPawnTag,
                NativeManifestStartupRestoreDispatched
                    ? STR("yes")
                    : STR("no-natural-restore"),
                runtime_preferences_contains_target
                    ? STR("yes")
                    : STR("no"),
                current_identity.empty()
                    ? STR("<none>")
                    : truncate_registry_probe_text(
                          current_identity,
                          500),
                intended_identity.empty()
                    ? STR("<none>")
                    : truncate_registry_probe_text(
                          intended_identity,
                          500),
                pawn && is_live_uobject_pointer(pawn)
                    ? pawn->GetFullName()
                    : STR("<null-or-stale>"),
                pawn_class_name.empty()
                    ? STR("<none>")
                    : pawn_class_name,
                pawn_class_is_target
                    ? STR("live-target-pawn-class")
                    : STR("live-pawn-current-identity"),
                now - NativeManifestStartupRestoreStartedMs);
            return;
        }

        if (NativeManifestStartupRestoreDispatched)
        {
            if (now >= NativeManifestStartupRestoreAuditDueMs &&
                (NativeManifestStartupRestorePollCount % 4 == 0))
            {
                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_AUDIT pawn_tag={} poll={} elapsed_ms={} runtime_preferences_match={} current_identity={} intended_identity={} pawn={} pawn_class={} mutation=no retry=no\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    NativeManifestStartupRestorePollCount,
                    now - NativeManifestStartupRestoreStartedMs,
                    runtime_preferences_contains_target
                        ? STR("yes")
                        : STR("no"),
                    current_identity.empty()
                        ? STR("<none>")
                        : truncate_registry_probe_text(
                              current_identity,
                              500),
                    intended_identity.empty()
                        ? STR("<none>")
                        : truncate_registry_probe_text(
                              intended_identity,
                              500),
                    pawn && is_live_uobject_pointer(pawn)
                        ? pawn->GetFullName()
                        : STR("<null-or-stale>"),
                    pawn_class_name.empty()
                        ? STR("<none>")
                        : pawn_class_name);
            }

            if (now >= NativeManifestStartupRestoreAuditDueMs +
                           post_dispatch_timeout_ms)
            {
                NativeManifestStartupRestoreComplete = true;
                NativeManifestStartupRestoreArmed = false;
                suit_log(
                    STR("{} ERROR restore: handover completed without target pawn pawn_tag={}\n"),
                    ModTag,
                    SavedNativeManifestPawnTag);
                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_END result=toggle-dispatched-but-target-not-observed pawn_tag={} elapsed_ms={} retry=no action=send-startup-restore-lines no-map-import=yes no-requestidentity=yes no-manual-possess=yes\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    now - NativeManifestStartupRestoreStartedMs);
            }
            return;
        }

        const bool game_save_objects_ready =
            preferences_save_data &&
            !saved_preferences.empty();
        // rf313: the story-party transition rewrites/clears Batman's
        // CharacterSelectSaveData selection while preserving the user's real
        // MostRecentIdentityByGroup preference. rf312 proved this exact split:
        // Thomas remained in persistent preferences and rendered correctly in
        // the frontend, but Gotham's DarkKnight configuration removed Thomas
        // from SelectionSaveData before this worker could dispatch. Treat the
        // persistent party preference as the save authority. SelectionSaveData
        // remains diagnostic evidence, but absence there is not a stale-sidecar
        // verdict. A conflicting persistent preference still rejects safely.
        if (game_save_objects_ready &&
            !persistent_preferences_confirm_target)
        {
            NativeManifestStartupRestoreComplete = true;
            NativeManifestStartupRestoreArmed = false;
            suit_log(
                STR("{} Saved custom suit not restored: pawn_tag={} reason=game-preference-changed\n"),
                ModTag,
                SavedNativeManifestPawnTag);
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_END result=stale-sidecar-rejected pawn_tag={} persistent_preferences_confirm={} character_selection_confirm={} toggle_called=no policy=persistent-party-preference-wins-over-sidecar-character-selection-is-diagnostic\n"),
                ModTag,
                SavedNativeManifestPawnTag,
                persistent_preferences_confirm_target
                    ? STR("yes")
                    : STR("no"),
                character_selection_confirms_target
                    ? STR("yes")
                    : STR("no"));
            return;
        }

        bool can_accept = false;
        const bool can_accept_called =
            party_member_live &&
            call_tt_party_member_bool_no_params_safe(
                party_member,
                STR("CanAcceptIdentityRequests"),
                can_accept,
                STR("native-manifest-startup-restore-preflight"));
        bool should_prevent_tag_within_party = false;
        const bool should_prevent_called =
            controller_component_live &&
            call_function_bool_return_no_params(
                controller_component,
                STR("ShouldPreventTagWithinParty"),
                should_prevent_tag_within_party);
        const bool base_ready_to_dispatch =
            metadata_exact &&
            generated_classes_ready &&
            game_save_objects_ready &&
            persistent_preferences_confirm_target &&
            party_system_live &&
            pawn_component_live &&
            party_member_live &&
            can_accept_called &&
            can_accept &&
            (!should_prevent_called ||
             !should_prevent_tag_within_party);

        UObject* active_suit_menu = nullptr;
        UObject* active_options_grid = nullptr;
        int32_t active_slots = 0;
        int32_t active_buttons = 0;
        const bool suit_menu_blocks_restore =
            base_ready_to_dispatch &&
            find_ready_batman_suit_grid(
                active_suit_menu,
                active_options_grid,
                active_slots,
                active_buttons,
                true) &&
            suit_grid_should_block_gameplay_or_handoff(
                active_suit_menu,
                active_options_grid,
                active_buttons);

        if (base_ready_to_dispatch &&
            !suit_menu_blocks_restore)
        {
            const StringType ready_signature =
                pawn->GetFullName() +
                STR("|") +
                pawn_component->GetFullName() +
                STR("|") +
                party_member->GetFullName() +
                STR("|") +
                current_identity;
            if (ready_signature ==
                NativeManifestStartupRestoreReadySignature)
            {
                ++NativeManifestStartupRestoreReadyObservations;
            }
            else
            {
                NativeManifestStartupRestoreReadySignature =
                    ready_signature;
                NativeManifestStartupRestoreReadySinceMs = now;
                NativeManifestStartupRestoreReadyObservations = 1;
            }
        }
        else
        {
            NativeManifestStartupRestoreReadySignature.clear();
            NativeManifestStartupRestoreReadySinceMs = 0;
            NativeManifestStartupRestoreReadyObservations = 0;
        }

        const bool gameplay_stable =
            NativeManifestStartupRestoreReadySinceMs != 0 &&
            now - NativeManifestStartupRestoreReadySinceMs >=
                stable_gameplay_ready_ms &&
            NativeManifestStartupRestoreReadyObservations >=
                stable_gameplay_observations;
        const bool ready_to_dispatch =
            base_ready_to_dispatch &&
            !suit_menu_blocks_restore &&
            gameplay_stable;

        if (!ready_to_dispatch)
        {
            if (NativeManifestStartupRestorePollCount == 1 ||
                NativeManifestStartupRestorePollCount % 120 == 0)
            {
                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_SAVE_AUTHORITY pawn_tag={} poll={} runtime_snapshot_contains_target={} saved_snapshot_contains_target={} runtime_snapshot_contains_darkknight={} saved_snapshot_contains_darkknight={} store_player_invariant_identities={} default_allow_preferred_identity={} identity_group_roots={} runtime_manifest_snapshots={} saved_manifest_snapshots={} policy=read-only-no-save-write-no-map-import-no-identity-request\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    NativeManifestStartupRestorePollCount,
                    exported_text_contains_tag_identity(
                        runtime_manifest_snapshots,
                        SavedNativeManifestPawnTag)
                        ? STR("yes")
                        : STR("no"),
                    exported_text_contains_tag_identity(
                        saved_manifest_snapshots,
                        SavedNativeManifestPawnTag)
                        ? STR("yes")
                        : STR("no"),
                    contains(
                        runtime_manifest_snapshots,
                        STR("Pawns.Playable.Batman.DarkKnight"))
                        ? STR("yes")
                        : STR("no"),
                    contains(
                        saved_manifest_snapshots,
                        STR("Pawns.Playable.Batman.DarkKnight"))
                        ? STR("yes")
                        : STR("no"),
                    player_preferences
                        ? truncate_registry_probe_text(
                              get_property_text(
                                  player_preferences,
                                  STR("bStoreIdentitiesForPlayerInvariantGroups")),
                              100)
                        : STR("<none>"),
                    player_preferences
                        ? truncate_registry_probe_text(
                              get_property_text(
                                  player_preferences,
                                  STR("bDefaultAllowPreferredIdentity")),
                              100)
                        : STR("<none>"),
                    player_preferences
                        ? truncate_registry_probe_text(
                              get_property_text(
                                  player_preferences,
                                  STR("IdentityGroupRoots")),
                              1200)
                        : STR("<none>"),
                    runtime_manifest_snapshots.empty()
                        ? STR("<empty>")
                        : truncate_registry_probe_text(
                              runtime_manifest_snapshots,
                              6000),
                    saved_manifest_snapshots.empty()
                        ? STR("<empty>")
                        : truncate_registry_probe_text(
                              saved_manifest_snapshots,
                              6000));

                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_WAIT pawn_tag={} poll={} elapsed_ms={} metadata_resolves_exact={} metadata_rooted_before={} metadata_rooted_after={} metadata_exact={} generated_classes_ready={} save_objects_ready={} persistent_preferences_confirm={} character_selection_confirm={} runtime_preferences_match={} player_controller={} controller_component={} party_system={} pawn_component={} party_member={} can_accept_called={} can_accept={} should_prevent_called={} should_prevent={} suit_menu_blocks={} stable_observations={} stable_elapsed_ms={} gameplay_stable={} current_identity={} action=wait-no-mutation\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    NativeManifestStartupRestorePollCount,
                    now - NativeManifestStartupRestoreStartedMs,
                    metadata_resolves_exact ? STR("yes") : STR("no"),
                    metadata_rooted_before ? STR("yes") : STR("no"),
                    metadata_rooted_after ? STR("yes") : STR("no"),
                    metadata_exact ? STR("yes") : STR("no"),
                    generated_classes_ready
                        ? STR("yes")
                        : STR("no"),
                    game_save_objects_ready ? STR("yes") : STR("no"),
                    persistent_preferences_confirm_target
                        ? STR("yes")
                        : STR("no"),
                    character_selection_confirms_target
                        ? STR("yes")
                        : STR("no"),
                    runtime_preferences_contains_target
                        ? STR("yes")
                        : STR("no"),
                    player_controller && is_live_uobject_pointer(player_controller)
                        ? player_controller->GetFullName()
                        : STR("<null-or-stale>"),
                    controller_component_live
                        ? controller_component->GetFullName()
                        : STR("<null-or-stale>"),
                    party_system_live
                        ? party_system->GetFullName()
                        : STR("<null-or-stale>"),
                    pawn_component_live
                        ? pawn_component->GetFullName()
                        : STR("<null-or-stale>"),
                    party_member_live
                        ? party_member->GetFullName()
                        : STR("<null-or-stale>"),
                    can_accept_called ? STR("yes") : STR("no"),
                    can_accept ? STR("true") : STR("false"),
                    should_prevent_called ? STR("yes") : STR("no"),
                    should_prevent_tag_within_party
                        ? STR("true")
                        : STR("false"),
                    suit_menu_blocks_restore ? STR("yes") : STR("no"),
                    NativeManifestStartupRestoreReadyObservations,
                    NativeManifestStartupRestoreReadySinceMs == 0
                        ? 0ULL
                        : now -
                              NativeManifestStartupRestoreReadySinceMs,
                    gameplay_stable ? STR("yes") : STR("no"),
                    current_identity.empty()
                        ? STR("<none>")
                        : truncate_registry_probe_text(
                              current_identity,
                              500));
            }

            if (NativeManifestStartupRestoreGameplayObservedMs != 0 &&
                now - NativeManifestStartupRestoreGameplayObservedMs >=
                    prerequisite_timeout_ms)
            {
                NativeManifestStartupRestoreComplete = true;
                NativeManifestStartupRestoreArmed = false;
                suit_log(
                    STR("{} ERROR restore: prerequisites timed out pawn_tag={} metadata_exact={} classes_ready={} save_ready={}\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    metadata_exact ? STR("yes") : STR("no"),
                    generated_classes_ready ? STR("yes") : STR("no"),
                    game_save_objects_ready ? STR("yes") : STR("no"));
                suit_vlog(
                    STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_END result=prerequisite-timeout pawn_tag={} polls={} elapsed_ms={} toggle_called=no action=send-startup-restore-wait-lines\n"),
                    ModTag,
                    SavedNativeManifestPawnTag,
                    NativeManifestStartupRestorePollCount,
                    now - NativeManifestStartupRestoreStartedMs);
            }
            return;
        }

        NativeManifestStartupRestoreDispatched = true;
        NativeManifestStartupRestoreAuditDueMs =
            now + 500ULL;

        suit_log(
            STR("{} Applying saved custom suit to gameplay: pawn_tag={}\n"),
            ModTag,
            SavedNativeManifestPawnTag);

        suit_vlog(
            STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_DISPATCH pawn_tag={} mod_id={} suit_id={} metadata={} party_system={} pawn_component={} controller_component={} party_member={} current_identity={} runtime_preferences_match={} saved_preferences_match=yes character_selection_match={} should_prevent_called={} should_prevent={} stable_observations={} stable_elapsed_ms={} operation=TogglePartyPawn instant_handover=no call_count=one policy=rf313-persistent-preference-authority-rf305-rf304-instant-crash-rollback-to-exact-rf303-stable-handover-no-runtime-map-import-no-requestidentity-no-manual-possess-no-save-write\n"),
            ModTag,
            SavedNativeManifestPawnTag,
            SavedNativeManifestModId.empty()
                ? STR("<none>")
                : SavedNativeManifestModId,
            SavedNativeManifestSuitId.empty()
                ? STR("<none>")
                : SavedNativeManifestSuitId,
            resolved_metadata->GetFullName(),
            party_system->GetFullName(),
            pawn_component->GetFullName(),
            controller_component_live
                ? controller_component->GetFullName()
                : STR("<null-or-stale>"),
            party_member->GetFullName(),
            current_identity.empty()
                ? STR("<none>")
                : truncate_registry_probe_text(
                      current_identity,
                      500),
            runtime_preferences_contains_target
                ? STR("yes")
                : STR("no"),
            character_selection_confirms_target
                ? STR("yes")
                : STR("no-diagnostic-only"),
            should_prevent_called ? STR("yes") : STR("no"),
            should_prevent_tag_within_party
                ? STR("true")
                : STR("false"),
            NativeManifestStartupRestoreReadyObservations,
            now - NativeManifestStartupRestoreReadySinceMs);

        const bool toggle_called =
            call_tt_party_system_toggle_party_pawn(
                party_system,
                pawn_component,
                SavedNativeManifestPawnTag,
                STR("rf305-schema3-cold-start-saved-custom-suit-stable-restore"),
                false);

        suit_vlog(
            STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_DISPATCH_END pawn_tag={} party_system_toggle_wrapper_called={} audit_armed={} retry=no next=observe-runtime-preferences-current-identity-and-pawn-class\n"),
            ModTag,
            SavedNativeManifestPawnTag,
            toggle_called ? STR("yes") : STR("no"),
            toggle_called ? STR("yes") : STR("no"));

        if (!toggle_called)
        {
            NativeManifestStartupRestoreComplete = true;
            NativeManifestStartupRestoreArmed = false;
            suit_log(
                STR("{} ERROR restore: party handover call failed pawn_tag={}\n"),
                ModTag,
                SavedNativeManifestPawnTag);
            suit_vlog(
                STR("{} SuitRuntime NATIVE_MANIFEST_STARTUP_RESTORE_END result=party-system-toggle-wrapper-failed pawn_tag={} retry=no action=send-dispatch-lines\n"),
                ModTag,
                SavedNativeManifestPawnTag);
        }
    }

    void install_native_manifest_process_event_callback()
    {
        if (NativeManifestProcessEventCallbackId != Hook::ERROR_ID)
        {
            return;
        }

        // CppUserModBase::on_update runs on the mod update thread and can race
        // Unreal's garbage collector. Dispatch from the first available
        // game-thread ProcessEvent instead. Do not require ReceiveTick: the
        // frontend controller does not implement it, so that event can arrive
        // after the saved class has already been consumed (or never arrive).
        // Use a post-hook so a restore that replaces a pawn cannot invalidate
        // the ProcessEvent invocation that selected this safe phase.
        NativeManifestProcessEventCallbackId =
            Hook::RegisterProcessEventPostCallback(
            [this](Hook::TCallbackIterationData<void>&, UObject*, UFunction*, void*)
            {
                const bool suit_selection_confirmation_pending =
                    NativeSuitSelectionConfirmationPending.load(
                        std::memory_order_acquire);
                if (NativeManifestProcessEventInternal ||
                    GlobalSelectionProcessEventTraceInternal ||
                    (!suit_selection_confirmation_pending &&
                     (!NativeManifestStartupClassPreloadArmed ||
                      NativeManifestStartupClassPreloadComplete) &&
                     (!NativeManifestStartupRestoreArmed ||
                      NativeManifestStartupRestoreComplete)))
                {
                    return;
                }

                if (NativeManifestProcessEventBusy.test_and_set(
                        std::memory_order_acquire))
                {
                    return;
                }

                NativeManifestProcessEventInternal = true;
                struct ResetInternalFlag
                {
                    bool& Flag;
                    std::atomic_flag& Busy;
                    ~ResetInternalFlag()
                    {
                        Flag = false;
                        Busy.clear(std::memory_order_release);
                    }
                } reset_internal_flag{
                    NativeManifestProcessEventInternal,
                    NativeManifestProcessEventBusy};

                tick_pending_native_suit_selection_confirmation();
                tick_native_manifest_startup_class_preload();
                tick_native_manifest_startup_restore();
            },
            {false, true, STR("LOTDKExpanded"), STR("NativeManifestStartupGameThread")});

        if (NativeManifestProcessEventCallbackId == Hook::ERROR_ID)
        {
            suit_log(
                STR("{} ERROR startup: could not register the ProcessEvent game-thread callback; saved-suit selection confirmation/preload/restore disabled\n"),
                ModTag);
            return;
        }

        suit_vlog(
            STR("{} Native manifest selection confirmation and startup work scheduled on game-thread ProcessEvent\n"),
            ModTag);
    }

    void uninstall_native_manifest_process_event_callback()
    {
        if (NativeManifestProcessEventCallbackId == Hook::ERROR_ID)
        {
            return;
        }

        const auto callback_id =
            std::exchange(
                NativeManifestProcessEventCallbackId,
                Hook::ERROR_ID);
        if (!Hook::UnregisterCallback(callback_id))
        {
            suit_log(
                STR("{} ERROR shutdown: could not unregister the native manifest ProcessEvent callback\n"),
                ModTag);
        }
    }


    auto on_unreal_init() -> void override
{
    CharacterModes->on_unreal_init();
    load_mod_settings();

    const auto& manifests =
        load_native_suit_mod_manifests_log_only();
    // All-suit warmup is deliberately disabled. The selected suit's classes are
    // preloaded on demand, which keeps startup and menu traversal lightweight.
    const bool saved_selection_loaded =
        NativeSchema3ManifestModeActive &&
        read_saved_native_manifest_selection(
            STR("startup"));

    // The mod update thread can overlap garbage collection and must not touch
    // UObject hash tables. An enabled ProcessEvent hook supplies the safe
    // game-thread phase in this installation.
    // Register the cold-load observers for the whole schema-3 session, even if
    // startup has no saved non-Batman selection yet. A user can select Catwoman,
    // Robin, or another character later in this same process; installing only
    // from startup state made that first live selection work in gameplay while
    // its subsequent cold cinematic load still missed the generated classes.
    // The callbacks already return immediately while no selection is saved.
    if (NativeSchema3ManifestModeActive)
    {
        install_native_manifest_lazy_scope_callbacks();
    }
    install_global_selection_process_event_trace();
    install_native_manifest_process_event_callback();

    size_t valid_manifests = 0;
    size_t enabled_suits = 0;
    for (const auto& manifest : manifests)
    {
        if (!manifest.Valid)
        {
            continue;
        }
        ++valid_manifests;
        enabled_suits += static_cast<size_t>(
            std::count_if(
                manifest.Suits.begin(),
                manifest.Suits.end(),
                [](const NativeSuitModManifestSuit& suit)
                {
                    return suit.Enabled && suit.Valid;
                }));
    }

    suit_log(
        STR("{} Ready ({}): mods={} suits={} saved_custom_suit={}\n"),
        ModTag,
        ReleaseBuild,
        valid_manifests,
        enabled_suits,
        saved_selection_loaded ? STR("yes") : STR("no"));

    if (enabled_suits == 0)
    {
        suit_log(
            STR("{} No valid schema-3 content found. Check each mod.json and the ERROR lines above.\n"),
            ModTag);
    }
}


    void on_lua_start(StringViewType name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main,
        LuaMadeSimple::Lua& async, LuaMadeSimple::Lua* hook) override
    {
        CharacterModes->on_lua_start(name, lua, main, async, hook);
    }
    void on_lua_stop(StringViewType name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main,
        LuaMadeSimple::Lua& async, LuaMadeSimple::Lua* hook) override
    {
        CharacterModes->on_lua_stop(name, lua, main, async, hook);
    }
    auto on_update() -> void override
{
}

};

#define LOTDKEXPANDED_API __declspec(dllexport)

using LuaStateOpaque = void;

extern "C"
{

    LOTDKEXPANDED_API CppUserModBase* start_mod()
    {
        return new LOTDKExpanded();
    }

    LOTDKEXPANDED_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
