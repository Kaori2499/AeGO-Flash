#ifndef L2DAE_IMPORT_PLUGIN_SOURCE
#define L2DAE_IMPORT_PLUGIN_SOURCE "../src/Plugin.cpp"
#endif
#define L2DAE_IMPORT_PREFERENCES_HEADER "../tests/ImportPreferencesStub.h"
#include L2DAE_IMPORT_PLUGIN_SOURCE
#include <iostream>
#include <array>
#include <unordered_map>
#include <vector>

namespace fixture {
unsigned checks = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
struct Allocation {
    size_t size = 0;
    unsigned locks = 0;
    bool hostValue = false;
};
std::unordered_map<PF_Handle, Allocation> memory;
std::unordered_map<PF_Handle, unsigned> lockAttempts;
std::vector<PF_ParamDef> parameters(1);
std::array<PF_ParamDef*, l2dae::kParameterCount> params{};
PF_InData in{};
PF_OutData out{};
PF_UtilCallbacks utils{};
PF_Handle failLock = nullptr;
unsigned failAttempt = 0, disposedBorrowed = 0, queued = 0, depth = 0;
bool cancelDialog = false, dialogError = false, queueError = false, preflightError = false;
l2dae::AnimationImportSelection chosen;
l2dae::ModelSelection modelChosen;
std::vector<l2dae::TimelineCommand> lastCommands;
l2dae::ModelSelection dialogCurrent;
l2dae::TimelineCommand lastCommand;
l2dae::RenderRequest lastRenderRequest;
unsigned renderCalls = 0;
std::vector<unsigned char> beforeMotion, beforeExpression;
bool inImport = false;
int preferenceCurve = 5, dialogPreferenceCurve = -1;
unsigned preferenceReads = 0, preferenceWrites = 0, dialogCalls = 0;
unsigned preferenceReadsBeforeImport = 0, queuedBeforeImport = 0;
bool preferenceCommitViolation = false;
float registeredOldTransition = 0, registeredDefaultTransition = 0;
bool registeredTransitionUsesOldValue = false;
void* const owner = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x51));

PF_Handle allocate(A_u_longlong size) {
    auto* data = new char[static_cast<size_t>(size)]{};
    auto* handle = new char*(data);
    auto result = reinterpret_cast<PF_Handle>(handle);
    memory.emplace(result, Allocation{static_cast<size_t>(size), 0, false});
    return result;
}
void* lock(PF_Handle handle) {
    auto& value = memory.at(handle);
    if (++lockAttempts[handle] == failAttempt && handle == failLock) return nullptr;
    ++value.locks;
    return *handle;
}
void unlock(PF_Handle handle) {
    auto& value = memory.at(handle);
    require(value.locks > 0, "unlock matches a live handle and lock");
    --value.locks;
}
void dispose(PF_Handle handle) {
    auto& value = memory.at(handle);
    if (value.hostValue && inImport) {
        ++disposedBorrowed;
        throw std::runtime_error("REGRESSION: import disposed an AE-owned borrowed parameter handle");
    }
    require(value.locks == 0, "cannot dispose a locked handle");
    delete[] static_cast<char*>(*handle);
    delete reinterpret_cast<char**>(handle);
    memory.erase(handle);
}
A_u_longlong sizeOf(PF_Handle handle) { return memory.at(handle).size; }
PF_Err abortNone(PF_ProgPtr) { return PF_Err_NONE; }
PF_Err addParam(PF_ProgPtr, PF_ParamIndex index, PF_ParamDef* def) {
    require(index == -1, "append stable parameter schema");
    parameters.push_back(*def);
    if (def->uu.id == 133) {
        registeredOldTransition = def->u.fs_d.value;
        registeredDefaultTransition = def->u.fs_d.dephault;
        registeredTransitionUsesOldValue = (def->flags & PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS) != 0;
    }
    if (def->param_type == PF_Param_FLOAT_SLIDER && (def->flags & PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS))
        parameters.back().u.fs_d.value = def->u.fs_d.dephault; // Fresh effect, not a missing parameter in an old project.
    if (def->param_type == PF_Param_ARBITRARY_DATA) {
        auto handle = def->u.arb_d.id == l2dae::kSelectionDiskId
            ? l2dae::newBank<l2dae::SelectionData>(&in, l2dae::readSelection(&in, def->u.arb_d.dephault))
            : l2dae::newBank<l2dae::ExpressionData>(&in, l2dae::readExpression(&in, def->u.arb_d.dephault));
        memory.at(handle).hostValue = true;
        parameters.back().u.arb_d.value = handle;
    }
    return PF_Err_NONE;
}
std::vector<unsigned char> bytes(PF_Handle handle) {
    if (!handle) return {};
    using namespace l2dae;
    if (memory.at(handle).size == sizeof(BankHeader)) {
        BankHeader header; std::memcpy(&header, *handle, sizeof(header));
        std::vector<unsigned char> result;
        if (header.kind == kSelectionDiskId) {
            const auto data = std::atomic_load(&static_cast<BankHolder<SelectionData>*>(header.holder)->snapshot);
            result.resize(wireSize(*data)); flattenBank(*data, result.data(), result.size());
        } else {
            const auto data = std::atomic_load(&static_cast<BankHolder<ExpressionData>*>(header.holder)->snapshot);
            result.resize(wireSize(*data)); flattenBank(*data, result.data(), result.size());
        }
        return result;
    }
    const auto* begin = static_cast<const unsigned char*>(*handle);
    return {begin, begin + memory.at(handle).size};
}
void resetOptions() {
    cancelDialog = dialogError = queueError = preflightError = false;
    failLock = nullptr; failAttempt = 0; lockAttempts.clear();
    for (size_t i = 1; i < parameters.size(); ++i) parameters[i].uu.change_flags = 0;
    out = {};
}
PF_Err callImport(bool expression) {
    const auto writesBefore = preferenceWrites;
    const auto rememberedBefore = preferenceCurve;
    const auto dialogCallsBefore = dialogCalls;
    preferenceReadsBeforeImport = preferenceReads;
    queuedBeforeImport = queued;
    const auto motion = parameters[l2dae::kSelection].u.arb_d.value;
    const auto expressions = parameters[l2dae::kExpressionSelection].u.arb_d.value;
    beforeMotion = bytes(motion); beforeExpression = bytes(expressions);
    if (motion) ++memory.at(motion).locks;
    if (expressions) ++memory.at(expressions).locks;
    inImport = true;
    PF_UserChangedParamExtra changed{};
    changed.param_index = expression ? l2dae::kImportExpression : l2dae::kOpenDialog;
    const PF_Err result = EffectMain(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr, &changed);
    inImport = false;
    // AE retains these original handles across the call, including automatic
    // unlock on return. Replacing or disposing them is a lifecycle violation.
    require(disposedBorrowed == 0, "REGRESSION: successful import must not free AE's original parameter handle");
    require(parameters[l2dae::kSelection].u.arb_d.value == motion &&
        parameters[l2dae::kExpressionSelection].u.arb_d.value == expressions,
        "import preserves both borrowed handle identities");
    if (motion) { require(memory.at(motion).locks == 1, "motion keeps only the outer host lock at return"); unlock(motion); }
    if (expressions) { require(memory.at(expressions).locks == 1, "expression keeps only the outer host lock at return"); unlock(expressions); }
    require(depth == 0, "import guard unwinds on every return path");
    require(!preferenceCommitViolation, "remembering a choice occurs only after queue acceptance and every bank/binding commit");
    if (dialogCalls != dialogCallsBefore)
        require(preferenceReads == preferenceReadsBeforeImport + 1, "each animation dialog reads the last confirmed preference once");
    if (result != PF_Err_NONE || !expression || cancelDialog || !chosen.hasMotion)
        require(preferenceWrites == writesBefore && preferenceCurve == rememberedBefore,
            "model-only expression-only cancellation and failed imports never save a motion preference");
    else
        require(preferenceWrites == writesBefore + 1 && preferenceCurve == chosen.transitionCurve,
            "a successfully committed motion remembers exactly one confirmed rebound choice");
    return result;
}
void setValues(const l2dae::SelectionData& motion, const l2dae::ExpressionData& expressions) {
    l2dae::ParameterWriteLock<l2dae::SelectionData>(&in, parameters[l2dae::kSelection].u.arb_d.value).write(std::make_shared<const l2dae::SelectionData>(motion));
    l2dae::ParameterWriteLock<l2dae::ExpressionData>(&in, parameters[l2dae::kExpressionSelection].u.arb_d.value).write(std::make_shared<const l2dae::ExpressionData>(expressions));
}
void confirmHostCanCopyAndDispose(PF_Handle handle, A_short id) {
    PF_ArbitraryH copy = nullptr;
    PF_ArbParamsExtra extra{};
    extra.id = id; extra.which_function = PF_Arbitrary_COPY_FUNC;
    extra.u.copy_func_params.src_arbH = handle; extra.u.copy_func_params.dst_arbPH = &copy;
    require(EffectMain(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0 && copy,
        "AE can copy the original handle after successful import returns");
    require(bytes(copy) == bytes(handle), "host's subsequent copy sees committed data");
    extra = {}; extra.id = id; extra.which_function = PF_Arbitrary_DISPOSE_FUNC;
    extra.u.dispose_func_params.arbH = copy;
    require(EffectMain(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0,
        "only the host's disposal callback frees its copied arbitrary data");
}

namespace audio {
unsigned checkouts = 0, gets = 0, checkins = 0, invalidCheckins = 0;
bool provide = false;
PF_Err checkoutError = PF_Err_NONE;
const auto token = reinterpret_cast<PF_LayerAudio>(static_cast<std::uintptr_t>(0x1234));
PF_Err checkout(PF_ProgPtr, PF_ParamIndex index, A_long, A_long, A_u_long,
    PF_UFixed, A_long, A_long, A_long, PF_LayerAudio* result) {
    require(index == l2dae::kAudioLayer, "audio checkout targets the audio layer parameter");
    ++checkouts; *result = provide ? token : nullptr; return checkoutError;
}
PF_Err get(PF_ProgPtr, PF_LayerAudio handle, PF_SndSamplePtr* samples, A_long* frames,
    PF_UFixed* rate, A_long* bytes, A_long* channels, A_long* format) {
    require(handle == token, "audio data access needs a nonnull returned handle");
    static std::int16_t silence[2]{};
    ++gets; *samples = silence; *frames = 1; *rate = 22050u * 65536u;
    *bytes = PF_SSS_2; *channels = PF_Channels_STEREO; *format = PF_SIGNED_PCM;
    return PF_Err_NONE;
}
PF_Err checkin(PF_ProgPtr, PF_LayerAudio handle) {
    if (!handle) { ++invalidCheckins; return PF_Err_INTERNAL_STRUCT_DAMAGED; }
    require(handle == token, "audio checkin releases the returned resource");
    ++checkins; return PF_Err_NONE;
}
void regressions() {
    using namespace l2dae;
    in.inter.checkout_layer_audio = checkout;
    in.inter.get_audio_data = get;
    in.inter.checkin_layer_audio = checkin;
    in.time_scale = 30; in.current_time = 30;
    parameters[kLipSync].u.bd.value = TRUE;
    RenderRequest request;
    require(readLipAudio(&in, params.data(), request) == 0 && checkouts == 1 &&
        !gets && !checkins && !invalidCheckins && !request.lipSyncEnabled,
        "REGRESSION: enabled None/null audio never gets or checks in null and preserves the motion mouth");
    for (PF_Err error : std::array<PF_Err, 2>{PF_Interrupt_CANCEL, PF_Err_OUT_OF_MEMORY}) {
        checkoutError = error;
        require(readLipAudio(&in, params.data(), request) == error && !checkins && !invalidCheckins,
            "null audio on host cancellation/error propagates without a checkin call");
    }
    checkoutError = PF_Err_NONE; provide = true;
    require(readLipAudio(&in, params.data(), request) == 0 && gets == 1 && checkins == 1 &&
        request.lipSyncEnabled && request.mouthOpen == 0,
        "actual silent audio still overrides the mouth and releases one resource");
    checkoutError = PF_Interrupt_CANCEL;
    require(readLipAudio(&in, params.data(), request) == PF_Interrupt_CANCEL && gets == 1 && checkins == 2,
        "cancellation with a returned resource releases that handle exactly once");
    parameters[kLipSync].u.bd.value = FALSE;
    const auto beforeDisabled = checkouts;
    request = {};
    require(readLipAudio(&in, params.data(), request) == 0 && checkouts == beforeDisabled && !request.lipSyncEnabled,
        "disabled lip sync bypasses every host audio callback");
    PF_UserChangedParamExtra changed{}; changed.param_index = kAudioLayer;
    parameters[kLipSync].uu.change_flags = 0;
    require(EffectMain(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr, &changed) == 0 &&
        !parameters[kLipSync].u.bd.value && !parameters[kLipSync].uu.change_flags,
        "initialization/None layer notifications cannot enable lip sync");
    in.inter.checkout_layer_audio = nullptr; in.inter.get_audio_data = nullptr;
    in.inter.checkin_layer_audio = nullptr;
}
}

void ambientRegressions() {
    using namespace l2dae;
    const auto savedParameters = parameters;
    const auto savedIn = in;
    const auto bank = parameters[kSelection].u.arb_d.value;
    const auto savedBank = readSelection(&in, bank);
    SelectionData selected;
    storePath(L"C:\\fixture\\model.model3.json", selected.model);
    ParameterWriteLock<SelectionData>(&in, bank).write(std::make_shared<const SelectionData>(selected));
    in.inter.abort = abortNone;
    in.current_time = 150; in.time_scale = 60;
    in.downsample_x = {1, 1}; in.downsample_y = {1, 1}; in.pixel_aspect_ratio = {1, 1};
    parameters[kManualTime].u.bd.value = TRUE;
    parameters[kMotionTimeA].u.fs_d.value = 4.25;
    parameters[kMotionTimeB].u.fs_d.value = 1.75;
    parameters[kSpeed].u.fs_d.value = 2;
    parameters[kStartTime].u.fs_d.value = 1;
    parameters[kLipSync].u.bd.value = FALSE;
    PF_Pixel8 pixels[4]{};
    PF_LayerDef world{};
    world.width = world.height = 2; world.rowbytes = 2 * sizeof(PF_Pixel8); world.data = pixels;
    const auto capture = [&]() {
        const auto before = renderCalls;
        require(EffectMain(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr) == 0 && renderCalls == before + 1,
            "production render callback forwards one request to the renderer");
    };
    capture();
    require(!lastRenderRequest.breathingEnabled && !lastRenderRequest.autoBlinkEnabled &&
        lastRenderRequest.ambientSeconds == 2.5, "old-project defaults disable both ambient effects with an independent host clock");
    parameters[kBreathing].u.bd.value = TRUE;
    capture();
    require(lastRenderRequest.breathingEnabled && !lastRenderRequest.autoBlinkEnabled &&
        lastRenderRequest.seconds == 4.25 && lastRenderRequest.secondsB == 1.75,
        "breathing key enables only breathing and leaves both motion times unchanged");
    parameters[kBreathing].u.bd.value = FALSE; parameters[kAutoBlink].u.bd.value = TRUE;
    capture();
    require(!lastRenderRequest.breathingEnabled && lastRenderRequest.autoBlinkEnabled,
        "blink key enables only automatic blinking");
    parameters[kBreathing].u.bd.value = TRUE;
    parameters[kMotionTimeA].u.fs_d.value = 34.2;
    parameters[kMotionTimeB].u.fs_d.value = 0.7;
    parameters[kSpeed].u.fs_d.value = 80;
    parameters[kStartTime].u.fs_d.value = -100;
    capture();
    require(lastRenderRequest.breathingEnabled && lastRenderRequest.autoBlinkEnabled &&
        lastRenderRequest.ambientSeconds == 2.5,
        "ambient clock is independent of A/B clip time, playback speed and start time");
    parameters[kManualTime].u.bd.value = FALSE;
    capture();
    require(lastRenderRequest.ambientSeconds == 2.5 && lastRenderRequest.seconds == 8200 &&
        lastRenderRequest.secondsB == 8200, "legacy automatic motion speed does not multiply the ambient clock");
    in.current_time = -15;
    capture();
    require(lastRenderRequest.ambientSeconds == -0.25, "negative host layer time is forwarded without changing either ambient switch");
    in.time_scale = 0;
    capture();
    require(lastRenderRequest.ambientSeconds == 0, "zero host time scale uses the existing safe zero-time fallback");
    params[kBreathing] = nullptr;
    capture();
    require(!lastRenderRequest.breathingEnabled && lastRenderRequest.autoBlinkEnabled,
        "missing breathing parameter safely disables only breathing");
    params[kBreathing] = &parameters[kBreathing]; params[kAutoBlink] = nullptr;
    capture();
    require(lastRenderRequest.breathingEnabled && !lastRenderRequest.autoBlinkEnabled,
        "missing blinking parameter safely disables only blinking");
    params[kBreathing] = nullptr;
    capture();
    require(!lastRenderRequest.breathingEnabled && !lastRenderRequest.autoBlinkEnabled,
        "missing ambient parameter values preserve old rendering behavior");
    parameters[kBreathAmount].u.fs_d.value = 25;
    parameters[kBreathPeriod].u.fs_d.value = 7;
    parameters[kBlinkStrength].u.fs_d.value = 50;
    parameters[kBlinkInterval].u.fs_d.value = 2;
    parameters[kBlinkDuration].u.fs_d.value = .75;
    capture();
    require(lastRenderRequest.breathingAmount == .25f && lastRenderRequest.breathingPeriod == 7 &&
        lastRenderRequest.blinkStrength == .5f && lastRenderRequest.blinkInterval == 2 &&
        lastRenderRequest.blinkDuration == .75, "visible ambient controls and retired blink strength/duration preserve host-evaluated legacy values");
    ParameterWriteLock<SelectionData>(&in, bank).write(savedBank);
    for (size_t i = 0; i < parameters.size(); ++i) {
        parameters[i] = savedParameters[i]; params[i] = &parameters[i];
    }
    in = savedIn; out = {};
}
}

namespace l2dae {
int lastImportTransitionCurve() noexcept {
    ++fixture::preferenceReads;
    return fixture::preferenceCurve;
}
bool rememberImportTransitionCurve(int curve) noexcept {
    using namespace fixture;
    ++preferenceWrites;
    try {
        if (!inImport || !chosen.hasMotion || curve < 3 || curve > 5 || curve != chosen.transitionCurve ||
            queued <= queuedBeforeImport || lastCommands.empty()) preferenceCommitViolation = true;
        const auto model = readSelection(&in, parameters[kSelection].u.arb_d.value);
        const auto expressions = readExpression(&in, parameters[kExpressionSelection].u.arb_d.value);
        if (widePath(model->model) != chosen.selection.modelPath || model->motions.size() != chosen.selection.motionPaths.size() ||
            expressions->paths.size() != chosen.selection.expressionPaths.size()) preferenceCommitViolation = true;
        else {
            for (size_t i = 0; i < model->motions.size(); ++i)
                if (widePath(model->motions[i]) != chosen.selection.motionPaths[i]) preferenceCommitViolation = true;
            for (size_t i = 0; i < expressions->paths.size(); ++i)
                if (widePath(expressions->paths[i]) != chosen.selection.expressionPaths[i]) preferenceCommitViolation = true;
        }
        for (const auto& command : lastCommands)
            if (parameters[command.expression ? kExpressionBinding : kTimelineBinding].u.fs_d.value != command.newBinding)
                preferenceCommitViolation = true;
        if (parameters[kImportTransitionFrames].u.fs_d.value != chosen.transitionFrames) preferenceCommitViolation = true;
    } catch (...) {
        preferenceCommitViolation = true;
        return false;
    }
    preferenceCurve = curve;
    return !preferenceCommitViolation;
}
bool initializeTimelineHost(SPBasicSuite*) noexcept { return true; }
void requireTimelineHost(SPBasicSuite*) {
    if (fixture::preflightError) throw std::runtime_error("fixture preflight failure");
}
TimelineImportScope::TimelineImportScope(SPBasicSuite* basic) {
    requireTimelineHost(basic); ownerWindow_ = fixture::owner; ++fixture::depth;
}
TimelineImportScope::~TimelineImportScope() noexcept { --fixture::depth; }
std::int32_t newTimelineBinding(std::int32_t previous) { return previous + 1; }
void enqueueTimelineCommands(SPBasicSuite*, const std::vector<TimelineCommand>& commands) {
    fixture::require(fixture::depth == 1, "idle stays suspended through queue and parameter commit");
    fixture::require(fixture::beforeMotion == fixture::bytes(fixture::parameters[kSelection].u.arb_d.value) &&
        fixture::beforeExpression == fixture::bytes(fixture::parameters[kExpressionSelection].u.arb_d.value),
        "queue preparation runs before either bank is modified");
    if (fixture::queueError) throw std::runtime_error("fixture script/queue preparation failure");
    fixture::lastCommands = commands; fixture::queued += static_cast<unsigned>(commands.size());
}
void enqueueTimelineCommand(SPBasicSuite* basic, const TimelineCommand& command) { enqueueTimelineCommands(basic, {command}); }
void shutdownTimelineHost() noexcept {}
bool showModelImportDialog(ModelSelection& selection, void* parent) {
    fixture::require(fixture::depth == 1 && parent == fixture::owner, "model dialog has AE owner and import guard");
    fixture::dialogCurrent = selection;
    if (fixture::dialogError) throw std::runtime_error("fixture dialog validation failure");
    if (fixture::cancelDialog) return false;
    selection = fixture::modelChosen; return true;
}
bool showAnimationImportDialog(const ModelSelection& current, AnimationImportSelection& result,
        void* parent, const RenderRequest* preview) {
    fixture::require(fixture::depth == 1 && parent == fixture::owner, "combined dialog has AE owner and import guard");
    fixture::require(preview != nullptr && result.transitionFrames == fixture::parameters[kImportTransitionFrames].u.fs_d.value,
        "dialog receives the stored transition preference and current ambient preview settings");
    fixture::require(result.transitionCurve == fixture::preferenceCurve,
        "dialog receives the last confirmed rebound choice rather than a hardcoded default");
    ++fixture::dialogCalls;
    fixture::dialogPreferenceCurve = result.transitionCurve;
    fixture::dialogCurrent = current;
    if (fixture::dialogError) throw std::runtime_error("fixture dialog validation failure");
    if (fixture::cancelDialog) return false;
    result = fixture::chosen; return true;
}
RenderResult render(const RenderRequest& request) {
    fixture::lastRenderRequest = request; ++fixture::renderCalls;
    RenderResult result;
    result.width = request.width; result.height = request.height;
    result.rgba.assign(static_cast<size_t>(request.width) * request.height * 4, 0);
    return result;
}
void releaseRenderer() {}
}

void storageRegressions() {
    using namespace l2dae; using namespace fixture;
    SelectionData large; storePath(LR"(C:\人物\模型.model3.json)", large.model);
    ExpressionData expressions;
    for (size_t i = 0; i < 4096; ++i) {
        std::string motion, expression;
        storePath(LR"(C:\动作\姿势🙂)" + std::to_wstring(i) + L".motion3.json", motion);
        storePath(LR"(C:\表情\微笑🙂)" + std::to_wstring(i) + L".exp3.json", expression);
        large.motions.push_back(motion); expressions.paths.push_back(expression);
    }
    const auto verify = [&](const auto& bank) {
        using T = std::decay_t<decltype(bank)>;
        const auto text = printableBank(bank);
        const auto scanned = scanBank<T>(text.data(), text.size());
        require(printableBank(scanned) == text, "4096 Unicode paths and supplementary-plane characters round trip through clipboard");
        std::vector<unsigned char> flat(wireSize(bank)+8,0xcd);
        flattenBank(bank, flat.data(), flat.size()-8);
        require(flat.back()==0xcd && flat[flat.size()-8]==0xcd, "variable wire preserves caller buffer guards");
        require(printableBank(unflattenBank<T>(flat.data(),flat.size()-8))==text,
            "4096 Unicode entries round trip through pointer-free saved wire");
        const auto rejectWire = [&](std::vector<unsigned char> value, size_t length) {
            bool rejected=false; try { (void)unflattenBank<T>(value.data(),length); } catch(...) {rejected=true;}
            require(rejected,"corrupt variable wire is rejected without a host allocation");
        };
        auto corrupted=flat; corrupted[12]=corrupted[13]=corrupted[14]=corrupted[15]=0xff;
        rejectWire(corrupted,flat.size()-8);
        corrupted=flat; corrupted[flat.size()-9]=0xff; rejectWire(corrupted,flat.size()-8);
        rejectWire(flat,flat.size()-9); rejectWire(flat,flat.size()-7);
        corrupted=flat;corrupted[8]=99;rejectWire(corrupted,flat.size()-8);
        const auto before=memory.size();
        auto handle=newBank<T>(&in,std::make_shared<const T>(bank));
        auto copy=newBank<T>(&in,readBank<T>(&in,handle));
        const auto old=readBank<T>(&in,copy);
        ParameterWriteLock<T>(&in,handle).write(std::make_shared<const T>());
        require(pathsOf(*readBank<T>(&in,handle)).empty() && printableBank(*readBank<T>(&in,copy))==text && printableBank(*old)==text,
            "copy and retained render snapshot preserve complete data after original bank import commit");
        disposeBank<T>(&in,handle);
        require(printableBank(*readBank<T>(&in,copy))==text,"copied bank survives disposal of original holder");
        disposeBank<T>(&in,copy);
        require(memory.size()==before,"immutable data owns no leaked host handles");
    };
    verify(large);verify(expressions);
    LegacySelectionData old1{};old1.version=1;strcpy_s(old1.model,"C:\\legacy\\model.model3.json");strcpy_s(old1.motion,"C:\\legacy\\Idle.motion3.json");
    LegacySelectionData2 old2{};old2.version=2;strcpy_s(old2.model,old1.model);
    for(size_t i=0;i<8;++i) strcpy_s(old2.motions[i],("C:\\motion"+std::to_string(i)+".motion3.json").c_str());
    LegacyExpressionData oldExpression{};oldExpression.version=1;
    for(size_t i=0;i<8;++i) strcpy_s(oldExpression.paths[i],("C:\\expression"+std::to_string(i)+".exp3.json").c_str());
    const auto makeWire=[](const auto& data,const char* magic){std::vector<unsigned char> wire(8+sizeof(data));std::memcpy(wire.data(),magic,8);std::memcpy(wire.data()+8,&data,sizeof(data));return wire;};
    const auto wire1=makeWire(old1,"L2DAE001"),wire2=makeWire(old2,"L2DAE002"),wireExpression=makeWire(oldExpression,"L2DEX001");
    const auto migrated1=unflattenBank<SelectionData>(wire1.data(),wire1.size());
    const auto migrated2=unflattenBank<SelectionData>(wire2.data(),wire2.size());
    const auto migratedExpression=unflattenBank<ExpressionData>(wireExpression.data(),wireExpression.size());
    require(migrated1.model==old1.model && migrated1.motions.size()==8 && migrated1.motions[0]==old1.motion && migrated1.motions.back().empty(),"v1 binary migrates motion to original slot1 with remaining original slots empty");
    require(migrated2.model==old2.model && migrated2.motions.size()==8 && migrated2.motions[7]==old2.motions[7],"v2 binary preserves every original motion slot");
    require(migratedExpression.paths.size()==8 && migratedExpression.paths[7]==oldExpression.paths[7],"v1 expression binary preserves every original expression slot");
    std::string oldText1="Live2DNative1|"+hexText(old1.model)+"|"+hexText(old1.motion);
    require(printableBank(scanBank<SelectionData>(oldText1.data(),oldText1.size()))==printableBank(migrated1),"old v1 clipboard migrates identically to binary");
    std::string oldText2="Live2DNative2|"+hexText(old2.model),oldExpressionText="Live2DExpression1|";
    for(size_t i=0;i<8;++i){oldText2+="|"+hexText(old2.motions[i]);if(i)oldExpressionText+='|';oldExpressionText+=hexText(oldExpression.paths[i]);}
    require(printableBank(scanBank<SelectionData>(oldText2.data(),oldText2.size()))==printableBank(migrated2),"old v2 clipboard migrates identically to binary");
    require(printableBank(scanBank<ExpressionData>(oldExpressionText.data(),oldExpressionText.size()))==printableBank(migratedExpression),"old expression clipboard migrates identically to binary");
    for(const std::string text : {"Live2DNative3|4294967295|","Live2DNative3|8|", "Live2DNative3|-1|", "Live2DNative3|1|00|aa", "Live2DNative3|1||ff", "Live2DNative3|0||"}) {
        bool rejected=false;try{(void)scanBank<SelectionData>(text.data(),text.size());}catch(...){rejected=true;}
        require(rejected,"malformed clipboard counts, UTF-8, null paths and extra fields rejected");
    }
}

int main() {
    using namespace l2dae; using namespace fixture;
    try {
        utils.host_new_handle = allocate; utils.host_get_handle_size = sizeOf;
        utils.host_lock_handle = lock; utils.host_unlock_handle = unlock; utils.host_dispose_handle = dispose;
        in.utils = &utils; in.inter.add_param = addParam;
        require(EffectMain(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) == 0, "production parameters");
        require(parameters.size() == kParameterCount, "complete appended schema");
        for (size_t i = 0; i < params.size(); ++i) params[i] = &parameters[i];
        for (auto index : {kMotionAIndex,kMotionBIndex,kExpressionAIndex,kExpressionBIndex})
            require((parameters[index].ui_flags & PF_PUI_NO_ECW_UI) &&
                !(parameters[index].ui_flags & PF_PUI_INVISIBLE) && !(parameters[index].flags & PF_ParamFlag_CANNOT_TIME_VARY),
                "dynamic timeline indices stay expression-capable instead of invisible host streams");
        for (auto index : {kBlinkStrength,kBlinkDuration})
            require((parameters[index].ui_flags & PF_PUI_INVISIBLE) &&
                !(parameters[index].flags & PF_ParamFlag_CANNOT_TIME_VARY),
                "retired blink options are absent from both panels without disabling saved animation streams");
        require(!(parameters[kBlinkInterval].ui_flags & (PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE)) &&
            !(parameters[kAutoBlink].ui_flags & (PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE)),
            "Auto blink and Blink interval remain visible");
        audio::regressions(); ambientRegressions(); storageRegressions();
        require(registeredTransitionUsesOldValue && registeredOldTransition == 15 && registeredDefaultTransition == 30,
            "transition registration preserves missing old-project value fifteen while declaring new default thirty");
        require(parameters[kImportTransitionFrames].u.fs_d.value == 30, "fresh effect uses exactly thirty composition frames");
        require(chosen.transitionFrames == 30 && chosen.transitionCurve == 5 && preferenceCurve == 5 && !preferenceReads && !preferenceWrites,
            "fresh dialog preferences default to thirty frames and pronounced rebound without touching any user file");
        resetOptions(); require(callImport(true) != 0 && queued == 0, "animation import first requires an independently imported model");
        require(std::string(out.return_msg).find(u8"请先导入 Live2D 模型") != std::string::npos &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out.return_msg, -1, nullptr, 0) > 0,
            "production PF error boundary emits readable Chinese UTF-8");
        modelChosen.modelPath = LR"(C:\角色\model.model3.json)";
        resetOptions(); require(callImport(false) == 0 && queued == 0, "model-only import never queues an animation");
        require(widePath(readSelection(&in, parameters[kSelection].u.arb_d.value)->model) == modelChosen.modelPath &&
            readSelection(&in, parameters[kSelection].u.arb_d.value)->motions.empty(), "model-only import leaves static pose and empty bank");
        confirmHostCanCopyAndDispose(parameters[kSelection].u.arb_d.value, kSelectionDiskId);
        chosen.selection.modelPath = modelChosen.modelPath;
        chosen.hasMotion = chosen.hasExpression = true;
        chosen.motion.slot = 1024; chosen.motion.duration = 2.75; chosen.motion.label = L"第1024个动作";
        chosen.expression.slot = 1024; chosen.expression.duration = 2.75; chosen.expression.label = L"第1024个表情";
        chosen.expression.append = false;
        for (size_t i = 0; i < 1024; ++i) {
            chosen.selection.motionPaths.push_back(LR"(C:\动作库\动作)" + std::to_wstring(i) + L".motion3.json");
            chosen.selection.expressionPaths.push_back(LR"(C:\表情库\表情)" + std::to_wstring(i) + L".exp3.json");
        }
        const auto oldSnapshot = readSelection(&in, parameters[kSelection].u.arb_d.value);
        resetOptions(); require(callImport(true) == 0 && lastCommands.size() == 2 && queued == 2,
            "1024-entry simultaneous motion/expression import queues one atomic batch");
        require(oldSnapshot->motions.empty(), "pre-import immutable snapshot survives commit unchanged");
        require(readSelection(&in, parameters[kSelection].u.arb_d.value)->motions.size() == 1024 &&
            readExpression(&in, parameters[kExpressionSelection].u.arb_d.value)->paths.size() == 1024,
            "both dynamic banks exceed the old eight-entry limit");
        require(lastCommands[0].slot == 1024 && !lastCommands[0].expression && lastCommands[1].expression &&
            lastCommands[0].transitionFrames == 30 && lastCommands[1].transitionFrames == 30,
            "batch preserves full indices, both types, names and thirty-frame transitions");
        require(lastCommands[0].transitionCurve == 5 && lastCommands[1].transitionCurve == 0 && dialogPreferenceCurve == 5,
            "first import starts with pronounced motion rebound and unchanged expression fades");
        for (int curve : {3, 4, 5}) {
            const auto priorPreference = preferenceCurve;
            const auto priorWrites = preferenceWrites;
            chosen.transitionCurve = curve;
            resetOptions(); require(callImport(true) == 0 && lastCommands.size() == 2,
                "each rebound amount commits one atomic motion/expression batch");
            require(lastCommands[0].transitionCurve == curve && lastCommands[1].transitionCurve == 0,
                "native import retains the chosen rebound amount only for motion");
            require(dialogPreferenceCurve == priorPreference && preferenceCurve == curve && preferenceWrites == priorWrites + 1,
                "accepted motion replaces the previous preference only after the complete transaction");
            resetOptions(); cancelDialog = true;
            require(callImport(true) == 0 && dialogPreferenceCurve == curve && preferenceWrites == priorWrites + 1,
                "the next opening receives the last confirmed choice and cancelling does not save again");
        }
        chosen.hasMotion = false;
        chosen.transitionCurve = 3;
        const auto beforeExpressionPreference = preferenceCurve;
        const auto beforeExpressionWrites = preferenceWrites;
        resetOptions(); require(callImport(true) == 0 && lastCommands.size() == 1 && lastCommands.front().expression &&
            lastCommands.front().transitionCurve == 0 && dialogPreferenceCurve == beforeExpressionPreference &&
            preferenceCurve == beforeExpressionPreference && preferenceWrites == beforeExpressionWrites,
            "expression-only import keeps the remembered motion amount and sends only a non-bounce expression command");
        chosen.hasMotion = true;
        chosen.transitionCurve = 6;
        resetOptions(); const auto beforeInvalidCurve = queued;
        require(callImport(true) != 0 && queued == beforeInvalidCurve &&
            bytes(parameters[kSelection].u.arb_d.value) == beforeMotion &&
            bytes(parameters[kExpressionSelection].u.arb_d.value) == beforeExpression,
            "invalid rebound selection cannot enqueue or mutate asset banks");
        chosen.transitionCurve = 3;
        confirmHostCanCopyAndDispose(parameters[kSelection].u.arb_d.value, kSelectionDiskId);
        confirmHostCanCopyAndDispose(parameters[kExpressionSelection].u.arb_d.value, kExpressionDiskId);
        // Dynamic index routing can reach >8 while all legacy popup streams keep their schema.
        in.inter.abort = abortNone; in.time_scale = 30; in.current_time = 30;
        in.downsample_x = {1,1}; in.downsample_y = {1,1}; in.pixel_aspect_ratio = {1,1};
        parameters[kMotionAIndex].u.fs_d.value = 1024;
        parameters[kExpressionAIndex].u.fs_d.value = 1024; parameters[kExpressionWeightA].u.fs_d.value = 100;
        PF_Pixel8 pixels[4]{}; PF_LayerDef world{}; world.width = world.height = 2; world.rowbytes = 2*sizeof(PF_Pixel8); world.data = pixels;
        require(EffectMain(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr) == 0 &&
            lastRenderRequest.motionPath == chosen.selection.motionPaths.back() &&
            lastRenderRequest.expressionPathA == chosen.selection.expressionPaths.back(), "renderer resolves dynamic motion and expression index 1024");
        parameters[kMotionAIndex].u.fs_d.value = 0; parameters[kExpressionAIndex].u.fs_d.value = 0;
        require(EffectMain(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr) == 0 &&
            lastRenderRequest.motionPath == chosen.selection.motionPaths.front(), "zero dynamic index uses the original saved popup stream");
        parameters[kExpressionWeightA].u.fs_d.value = 0;
        // Failure after either host lock or before batch acceptance leaves every value unchanged.
        chosen.transitionFrames = 24;
        chosen.selection.motionPaths.push_back(LR"(C:\动作库\next.motion3.json)"); chosen.motion.slot = 1025;
        chosen.selection.expressionPaths.push_back(LR"(C:\表情库\next.exp3.json)"); chosen.expression.slot = 1025;
        for (int failure = 0; failure < 6; ++failure) {
            resetOptions(); const auto oldQueued = queued;
            const auto oldMotionBinding = parameters[kTimelineBinding].u.fs_d.value;
            const auto oldExpressionBinding = parameters[kExpressionBinding].u.fs_d.value;
            cancelDialog = failure == 0; dialogError = failure == 1; queueError = failure == 2; preflightError = failure == 3;
            if (failure >= 4) { failLock = parameters[failure == 4 ? kSelection : kExpressionSelection].u.arb_d.value; failAttempt = 2; }
            const auto result = callImport(true);
            require(failure == 0 ? result == 0 : result != 0, "cancel or injected import failure propagates");
            require(bytes(parameters[kSelection].u.arb_d.value) == beforeMotion &&
                bytes(parameters[kExpressionSelection].u.arb_d.value) == beforeExpression, "failed paired import never partially changes either bank");
            require(queued == oldQueued && parameters[kTimelineBinding].u.fs_d.value == oldMotionBinding &&
                parameters[kExpressionBinding].u.fs_d.value == oldExpressionBinding && parameters[kImportTransitionFrames].u.fs_d.value == 30,
                "failed batch leaves queue, both bindings and transition preference untouched");
            for (auto index : {kSelection,kExpressionSelection,kTimelineBinding,kExpressionBinding,kImportTransitionFrames})
                require(parameters[index].uu.change_flags == 0, "failed batch marks no host value changed");
        }
        resetOptions(); require(callImport(true) == 0 && parameters[kImportTransitionFrames].u.fs_d.value == 24,
            "accepted batch persists its transition preference only after successful enqueue");
        modelChosen.modelPath = LR"(C:\角色\other.model3.json)";
        resetOptions(); require(callImport(false) != 0 && bytes(parameters[kSelection].u.arb_d.value) == beforeMotion,
            "replacing a model cannot invalidate existing animation clips");
        modelChosen.modelPath = chosen.selection.modelPath;
        resetOptions(); require(callImport(false) == 0 && parameters[kSelection].uu.change_flags == 0,
            "reimporting the same model preserves both banks and existing clips");
        // A malicious or buggy dialog may not silently change old slot meanings.
        auto goodPath = chosen.selection.motionPaths.front(); chosen.selection.motionPaths.front() = LR"(C:\different.motion3.json)";
        resetOptions(); require(callImport(true) != 0 && bytes(parameters[kSelection].u.arb_d.value) == beforeMotion,
            "existing nonempty bank indices cannot be overwritten by import"); chosen.selection.motionPaths.front() = goodPath;
        chosen.selection.expressionPaths.back().assign(1, static_cast<wchar_t>(0xd800));
        resetOptions(); require(callImport(true) != 0 && bytes(parameters[kSelection].u.arb_d.value) == beforeMotion &&
            bytes(parameters[kExpressionSelection].u.arb_d.value) == beforeExpression,
            "invalid expression Unicode rejects the entire paired batch before mutation");
        chosen.selection.expressionPaths.back() = LR"(C:\表情库\next.exp3.json)";
        const auto normalHandle = parameters[kSelection].u.arb_d.value;
        const auto legacyHandle = allocate(sizeof(LegacySelectionData));
        auto* legacy = static_cast<LegacySelectionData*>(*legacyHandle); legacy->version = 1;
        const char* legacyModel = "C:\\legacy\\model.model3.json";
        strcpy_s(legacy->model, legacyModel); strcpy_s(legacy->motion, "C:\\legacy\\Idle.motion3.json");
        memory.at(legacyHandle).hostValue = true;
        parameters[kSelection].u.arb_d.value = legacyHandle;
        const auto expressionHandle = parameters[kExpressionSelection].u.arb_d.value;
        chosen.selection = dialogSelection(*readSelection(&in,legacyHandle), *readExpression(&in,expressionHandle));
        chosen.hasMotion = true; chosen.hasExpression = false; chosen.motion.slot = 1;
        resetOptions(); require(callImport(true) != 0 && std::strstr(out.return_msg,u8"保存数据需要迁移") &&
            bytes(legacyHandle) == beforeMotion, "unexpected borrowed legacy allocation is never resized or freed on import");
        std::vector<unsigned char> oldWire(8 + sizeof(LegacySelectionData)); std::memcpy(oldWire.data(), "L2DAE001", 8);
        std::memcpy(oldWire.data()+8, legacy, sizeof(*legacy));
        PF_ArbitraryH migrated = nullptr; PF_ArbParamsExtra extra{};
        extra.id = kSelectionDiskId; extra.which_function = PF_Arbitrary_UNFLATTEN_FUNC;
        extra.u.unflatten_func_params.buf_sizeLu = static_cast<A_u_long>(oldWire.size());
        extra.u.unflatten_func_params.flat_dataPV = oldWire.data(); extra.u.unflatten_func_params.arbPH = &migrated;
        resetOptions(); require(EffectMain(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&extra)==0 &&
            memory.at(migrated).size==sizeof(BankHeader), "old project unflatten migrates to a safe owning header before UI");
        memory.at(migrated).hostValue=true; parameters[kSelection].u.arb_d.value=migrated;
        resetOptions(); require(callImport(true)==0, "migrated legacy model accepts animation while keeping its host handle");
        confirmHostCanCopyAndDispose(migrated,kSelectionDiskId);
        parameters[kSelection].u.arb_d.value=normalHandle; disposeBank<SelectionData>(&in, legacyHandle); disposeBank<SelectionData>(&in,migrated);
        for (auto index : {kSelection, kExpressionSelection}) {
            extra={};extra.id=parameters[index].u.arb_d.id;extra.which_function=PF_Arbitrary_DISPOSE_FUNC;
            extra.u.dispose_func_params.arbH=parameters[index].u.arb_d.value;
            require(EffectMain(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&extra)==0,"host disposes retained original value exactly once");
            extra.u.dispose_func_params.arbH=parameters[index].u.arb_d.dephault;
            require(EffectMain(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&extra)==0,"host disposes independent default value");
        }
        require(memory.empty() && depth==0 && disposedBorrowed==0,"no leaked handles, outer locks or scopes");
        std::cout << "PASS: " << checks << " production import/storage/audio/ambient checks; isolated in-memory preferences; new thirty-frame and old fifteen-frame defaults; confirmed motion preference commits; paired1024-entry banks; fixed borrowed handles; immutable snapshots; transactional cancellation/errors; legacy migration; dynamic index routing; simplified blink UI and legacy ambient values.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
}
