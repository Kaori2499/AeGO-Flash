#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Plugin.h"
#include "Dialog.h"
#include "Renderer.h"
#include "TimelineHost.h"
#include "AudioEnvelope.h"
#include "UiText.h"
#include "HostText.h"
#include "HostProcess.h"
#ifndef L2DAE_IMPORT_PREFERENCES_HEADER
#define L2DAE_IMPORT_PREFERENCES_HEADER "ImportPreferences.h"
#endif
#include L2DAE_IMPORT_PREFERENCES_HEADER
#include "AE_EffectSuites.h"
#include <atomic>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <memory>
#include <vector>
#include <mutex>

namespace l2dae {
namespace {
#include "PathBank.inc"

// Registration can be skipped when AE reuses cached plug-in metadata. Resolve
// the running host independently before any native text is exposed to AE.
// Render threads only read the published code page; they never query host APIs.
std::atomic<UINT> gHostCodePage{CP_UTF8};
HostVersion gHostVersion{};
char gHostLanguage[PF_APP_LANG_TAG_SIZE]{};
std::mutex gHostTextMutex;
void initializeHostText(SPBasicSuite* basic, const char* version = nullptr) noexcept {
    try {
        const auto processVersion = currentProcessHostVersion();
        const auto registrationVersion = parseHostVersion(version);
        char language[PF_APP_LANG_TAG_SIZE]{};
        bool languageValid = false;
        if (basic && basic->AcquireSuite && basic->ReleaseSuite) {
            const PFAppSuite6* app = nullptr;
            if (!basic->AcquireSuite(kPFAppSuite, kPFAppSuiteVersion6, reinterpret_cast<const void**>(&app))) {
                languageValid = app && app->PF_AppGetLanguage && !app->PF_AppGetLanguage(language) &&
                    language[0] && std::memchr(language, '\0', sizeof(language));
                (void)basic->ReleaseSuite(kPFAppSuite, kPFAppSuiteVersion6);
            }
        }
        const std::lock_guard<std::mutex> guard(gHostTextMutex);
        if (processVersion.valid) gHostVersion = processVersion;
        else if (registrationVersion.valid) gHostVersion = registrationVersion;
        if (languageValid) {
            std::memcpy(gHostLanguage, language, sizeof(gHostLanguage));
            // A failed callback can leave en_US in the buffer. Only a validated
            // tag may change the UI language, and an empty tag stays put.
            setUiLanguageFromHost(gHostLanguage);
        }
        gHostCodePage.store(hostTextCodePage(gHostVersion, gHostLanguage), std::memory_order_relaxed);
    } catch (...) {
        // Failed optional discovery must not destroy a previously valid context.
    }
}
std::string hostText(const wchar_t* text) {
    return encodeHostText(text, gHostCodePage.load(std::memory_order_relaxed));
}
const char* hostLabel(const wchar_t* text) {
    // AE retains button/checkbox/popup pointers beyond setup. Each encoding has
    // separate immutable storage; map insertions never invalidate prior labels.
    static std::mutex mutex;
    static std::map<std::pair<UINT, std::wstring>, std::string> labels;
    std::lock_guard<std::mutex> lock(mutex);
    const auto key = std::make_pair(gHostCodePage.load(std::memory_order_relaxed), std::wstring(text));
    auto found = labels.find(key);
    if (found == labels.end()) found = labels.emplace(key, encodeHostText(key.second, key.first)).first;
    return found->second.c_str();
}

void parameterName(PF_ParamDef& value, const wchar_t* name, A_long diskId) {
    const auto encoded = hostText(name);
    // Never truncate a multibyte character inside AE's fixed-size name field.
    if (encoded.size() >= sizeof(value.PF_DEF_NAME))
        throw std::runtime_error("The effect parameter name is too long.");
    std::memcpy(value.PF_DEF_NAME, encoded.c_str(), encoded.size() + 1);
    value.uu.id = diskId;
}

PF_Err addSlider(PF_InData* in_data, const wchar_t* name, A_long diskId,
                 float minimum, float maximum, float sliderMin, float sliderMax,
                 float initial, A_short precision, bool animatable = true) {
    PF_ParamDef def{};
    parameterName(def, name, diskId);
    def.param_type = PF_Param_FLOAT_SLIDER;
    def.flags = animatable ? 0 : PF_ParamFlag_CANNOT_TIME_VARY;
    // Internal animation streams remain available in the Timeline for legacy
    // projects, while the Effect Controls panel shows the simple clip workflow.
    if (diskId == 104 || diskId == 105 || (diskId >= 112 && diskId <= 114) || diskId == 120 || diskId == 121)
        def.ui_flags = PF_PUI_NO_ECW_UI;
    if (diskId >= 134 && diskId <= 137) {
        // These streams must remain available to AE timeline expressions.
        // PF_PUI_INVISIBLE also disables canSetExpression in the real host.
        def.ui_flags = PF_PUI_NO_ECW_UI;
    }
    if (diskId == 115 || diskId == 122 || diskId == 130 || diskId == 132)
        def.ui_flags = PF_PUI_INVISIBLE;
    // Blink strength/duration are retired UI controls. Keep their original
    // streams and time-varying flags so existing saved values and keys can still
    // reach readAmbient; fresh effects retain 100% strength and 0.3s duration.
    def.u.fs_d.valid_min = minimum;
    def.u.fs_d.valid_max = maximum;
    def.u.fs_d.slider_min = sliderMin;
    def.u.fs_d.slider_max = sliderMax;
    def.u.fs_d.value = def.u.fs_d.dephault = initial;
    if (diskId == 133) {
        // New/reset effects use 30. Older projects that lack this stream keep
        // the former 15-frame value; existing saved values remain untouched.
        def.flags |= PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS;
        def.u.fs_d.value = 15;
    }
    def.u.fs_d.precision = precision;
    return PF_ADD_PARAM(in_data, -1, &def);
}

PF_Err addMotionSlot(PF_InData* in_data, const wchar_t* name, A_long diskId, A_short initial) {
    PF_ParamDef def{};
    parameterName(def, name, diskId);
    def.param_type = PF_Param_POPUP;
    // The slot itself can be keyframed, but changes are discrete (Hold).
    def.flags = PF_ParamFlag_CANNOT_INTERP;
    def.ui_flags = PF_PUI_NO_ECW_UI;
    def.u.pd.num_choices = static_cast<A_short>(kLegacySlots);
    def.u.pd.value = def.u.pd.dephault = initial;
    const auto choices = hostLabel(uiText(
        L"Slot 1|Slot 2|Slot 3|Slot 4|Slot 5|Slot 6|Slot 7|Slot 8",
        L"槽位 1|槽位 2|槽位 3|槽位 4|槽位 5|槽位 6|槽位 7|槽位 8"));
    def.u.pd.u.namesptr = choices;
    return PF_ADD_PARAM(in_data, -1, &def);
}

PF_Err addGroup(PF_InData* in_data, const wchar_t* name, A_long diskId, bool end, bool collapsed) {
    PF_ParamDef def{};
    parameterName(def, name, diskId);
    def.param_type = end ? PF_Param_GROUP_END : PF_Param_GROUP_START;
    if (!end && collapsed) def.flags = PF_ParamFlag_START_COLLAPSED;
    return PF_ADD_PARAM(in_data, -1, &def);
}
PF_Err addButton(PF_InData* in_data, const wchar_t* name, A_long diskId, const char* label) {
    PF_ParamDef def{};
    parameterName(def, name, diskId);
    def.param_type = PF_Param_BUTTON;
    def.u.button_d.u.namesptr = label;
    def.flags = PF_ParamFlag_SUPERVISE;
    def.ui_flags = PF_PUI_STD_CONTROL_ONLY;
    return PF_ADD_PARAM(in_data, -1, &def);
}
PF_Err addCheckbox(PF_InData* in_data, const wchar_t* name, A_long diskId,
                   PF_ParamFlags flags, PF_Boolean initial, bool hidden = false) {
    PF_ParamDef def{};
    parameterName(def, name, diskId);
    def.param_type = PF_Param_CHECKBOX;
    def.flags = flags;
    if (hidden) def.ui_flags = PF_PUI_NO_ECW_UI;
    def.u.bd.u.nameptr = hostLabel(uiText(L"Enable", L"启用"));
    def.u.bd.value = def.u.bd.dephault = initial;
    return PF_ADD_PARAM(in_data, -1, &def);
}

PF_Err setupParameters(PF_InData* in_data, PF_OutData* out_data) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def{};
    // Registration order mirrors ParameterIndex. Original disk IDs, defaults,
    // parameter types and animation flags preserve saved projects and keyframes.
    if ((err = addGroup(in_data, uiText(L"Model & Animation", L"模型与动画"), 201, false, false))) return err;
    if ((err = addButton(in_data, uiText(L"Import Model", L"导入模型"), 101, hostLabel(uiText(L"Import Model...", L"导入模型…"))))) return err;
    def = {};
    parameterName(def, uiText(L"Model Selection", L"模型选择"), kSelectionDiskId);
    def.param_type = PF_Param_ARBITRARY_DATA;
    def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
    def.ui_flags = PF_PUI_NO_ECW_UI;
    def.u.arb_d.id = kSelectionDiskId;
    def.u.arb_d.dephault = newBank<SelectionData>(in_data, std::make_shared<const SelectionData>());
    err = PF_ADD_PARAM(in_data, -1, &def);
    if (err) { if (def.u.arb_d.id == kSelectionDiskId) disposeBank<SelectionData>(in_data, def.u.arb_d.dephault); else disposeBank<ExpressionData>(in_data, def.u.arb_d.dephault); return err; }
    // AE owns the default handle after successful PF_ADD_PARAM.

    if ((err = addButton(in_data, uiText(L"Import Clips", L"导入动作与表情"), 116, hostLabel(uiText(L"Import Motions & Expressions...", L"导入动作与表情…"))))) return err;
    if ((err = addSlider(in_data, uiText(L"Default Transition (frames)", L"默认过渡 (帧)"), 133, 0, 100000, 0, 60, 30, 0, false))) return err;
    if ((err = addGroup(in_data, L"", 202, true, false))) return err;
    if ((err = addGroup(in_data, uiText(L"Position & Scale", L"位置与大小"), 203, false, false))) return err;
    if ((err = addSlider(in_data, uiText(L"Scale (%)", L"缩放 (%)"), 106, 1, 10000, 1, 200, 100, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Horizontal Offset (px)", L"水平偏移 (像素)"), 107, -100000, 100000, -2000, 2000, 0, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Vertical Offset (px)", L"垂直偏移 (像素)"), 108, -100000, 100000, -2000, 2000, 0, 1))) return err;
    if ((err = addGroup(in_data, L"", 204, true, false))) return err;
    if ((err = addGroup(in_data, uiText(L"Audio & Lip Sync", L"声音与口型"), 205, false, true))) return err;
    def = {};
    parameterName(def, uiText(L"Audio Layer", L"音频图层"), 123);
    def.param_type = PF_Param_LAYER;
    def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
    def.u.ld.dephault = PF_LayerDefault_NONE;
    if ((err = PF_ADD_PARAM(in_data, -1, &def))) return err;
    if ((err = addCheckbox(in_data, uiText(L"Lip Sync to Audio", L"声音同步口型"), 124, PF_ParamFlag_CANNOT_TIME_VARY, FALSE, false))) return err;
    if ((err = addSlider(in_data, uiText(L"Lip Sync Sensitivity (%)", L"口型灵敏度 (%)"), 125, 0, 500, 0, 500, 100, 1))) return err;
    if ((err = addGroup(in_data, L"", 206, true, false))) return err;
    if ((err = addGroup(in_data, uiText(L"Breathing & Blink", L"呼吸与眨眼"), 207, false, true))) return err;
    if ((err = addCheckbox(in_data, uiText(L"Breathing", L"呼吸动画"), 126, PF_ParamFlag_CANNOT_INTERP, FALSE, false))) return err;
    if ((err = addSlider(in_data, uiText(L"Breathing Amount (%)", L"呼吸幅度 (%)"), 128, 0, 100, 0, 100, 100, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Breathing Period (sec)", L"呼吸周期 (秒)"), 129, .1f, 60, .1f, 10, 4, 2))) return err;
    if ((err = addCheckbox(in_data, uiText(L"Auto Blink", L"自动眨眼"), 127, PF_ParamFlag_CANNOT_INTERP, FALSE, false))) return err;
    if ((err = addSlider(in_data, uiText(L"Blink Interval (sec)", L"眨眼间隔 (秒)"), 131, .2f, 60, .2f, 10, 4, 2))) return err;
    if ((err = addGroup(in_data, L"", 208, true, false))) return err;
    // Hidden data and legacy streams keep their original persistent IDs.
    if ((err = addCheckbox(in_data, uiText(L"Loop Motion", L"循环动作"), 103, PF_ParamFlag_CANNOT_TIME_VARY, TRUE, true))) return err;
    if ((err = addSlider(in_data, uiText(L"Playback Speed", L"播放速度"), 104, 0, 100, 0, 4, 1, 2, false))) return err;
    if ((err = addSlider(in_data, uiText(L"Start Time (sec)", L"起始时间 (秒)"), 105, -86400, 86400, 0, 60, 0, 3, false))) return err;
    if ((err = addMotionSlot(in_data, uiText(L"Motion A Slot", L"动作 A 槽位"), 109, 1))) return err;
    if ((err = addMotionSlot(in_data, uiText(L"Motion B Slot", L"动作 B 槽位"), 110, 2))) return err;
    if ((err = addCheckbox(in_data, uiText(L"Motion Time Keys", L"动作时间关键帧"), 111, PF_ParamFlag_CANNOT_TIME_VARY, FALSE, true))) return err;
    if ((err = addSlider(in_data, uiText(L"Motion A Time (sec)", L"动作 A 时间 (秒)"), 112, 0, 86400, 0, 60, 0, 3))) return err;
    if ((err = addSlider(in_data, uiText(L"Motion B Time (sec)", L"动作 B 时间 (秒)"), 113, 0, 86400, 0, 60, 0, 3))) return err;
    if ((err = addSlider(in_data, uiText(L"Motion A to B Blend (%)", L"动作 A 到 B 过渡 (%)"), 114, 0, 110, 0, 110, 0, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Motion Track Binding", L"动作轨道绑定"), 115, 0, 2147483648.0f, 0, 2147483648.0f, 0, 0, false))) return err;
    def = {};
    parameterName(def, uiText(L"Expression Selection", L"表情选择"), kExpressionDiskId);
    def.param_type = PF_Param_ARBITRARY_DATA;
    def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
    def.ui_flags = PF_PUI_NO_ECW_UI;
    def.u.arb_d.id = kExpressionDiskId;
    def.u.arb_d.dephault = newBank<ExpressionData>(in_data, std::make_shared<const ExpressionData>());
    err = PF_ADD_PARAM(in_data, -1, &def);
    if (err) { if (def.u.arb_d.id == kSelectionDiskId) disposeBank<SelectionData>(in_data, def.u.arb_d.dephault); else disposeBank<ExpressionData>(in_data, def.u.arb_d.dephault); return err; }
    if ((err = addMotionSlot(in_data, uiText(L"Expression A Slot", L"表情 A 槽位"), 118, 1))) return err;
    if ((err = addMotionSlot(in_data, uiText(L"Expression B Slot", L"表情 B 槽位"), 119, 2))) return err;
    if ((err = addSlider(in_data, uiText(L"Expression A Amount (%)", L"表情 A 强度 (%)"), 120, 0, 100, 0, 100, 0, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Expression B Amount (%)", L"表情 B 强度 (%)"), 121, 0, 100, 0, 100, 0, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Expression Binding", L"表情轨道绑定"), 122, 0, 2147483648.0f, 0, 2147483648.0f, 0, 0, false))) return err;
    if ((err = addSlider(in_data, uiText(L"Blink Strength (%)", L"眨眼强度 (%)"), 130, 0, 100, 0, 100, 100, 1))) return err;
    if ((err = addSlider(in_data, uiText(L"Blink Duration (sec)", L"眨眼时长 (秒)"), 132, .02f, 2, .02f, 2, .3f, 2))) return err;
    // Preserve old popup disk IDs, types and keyframes. Zero chooses the legacy
    // stream; current timeline clips use these exact integer float streams.
    for (A_long id : {134L, 135L, 136L, 137L}) {
        const wchar_t* names[] = {
            uiText(L"Motion A Index", L"动作 A 编号"), uiText(L"Motion B Index", L"动作 B 编号"),
            uiText(L"Expression A Index", L"表情 A 编号"), uiText(L"Expression B Index", L"表情 B 编号")};
        if ((err = addSlider(in_data, names[id - 134], id, 0, static_cast<float>(kMaximumIndex), 0,
                static_cast<float>(kMaximumIndex), 0, 0))) return err;
    }
    out_data->num_params = kParameterCount;
    return PF_Err_NONE;
}

void readAmbient(PF_ParamDef* params[], RenderRequest& request) {
    request.breathingEnabled = params[kBreathing] && params[kBreathing]->u.bd.value != 0;
    request.autoBlinkEnabled = params[kAutoBlink] && params[kAutoBlink]->u.bd.value != 0;
    const auto value = [&](ParameterIndex index, double fallback, double minimum, double maximum) {
        const double v = params[index] ? params[index]->u.fs_d.value : fallback;
        if (!std::isfinite(v)) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
        return std::clamp(v, minimum, maximum);
    };
    request.breathingAmount = static_cast<float>(value(kBreathAmount, 100, 0, 100) / 100.0);
    request.breathingPeriod = value(kBreathPeriod, 4, .1, 60);
    request.blinkStrength = static_cast<float>(value(kBlinkStrength, 100, 0, 100) / 100.0);
    request.blinkInterval = value(kBlinkInterval, 4, .2, 60);
    request.blinkDuration = value(kBlinkDuration, .3, .02, 2);
    // AE stores slider values as float; retain the old exact 0.3s ambient curve.
    if (request.blinkDuration == static_cast<double>(.3f)) request.blinkDuration = .3;
}
ModelSelection dialogSelection(const SelectionData& model, const ExpressionData& expressions) {
    ModelSelection result; result.modelPath = widePath(model.model);
    for (const auto& path : model.motions) result.motionPaths.push_back(widePath(path));
    for (const auto& path : expressions.paths) result.expressionPaths.push_back(widePath(path));
    return result;
}
std::int32_t currentBinding(const PF_ParamDef* parameter) {
    if (!parameter) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
    const double value = parameter->u.fs_d.value;
    if (!std::isfinite(value) || value < 0 || value > 2147483647.0 || std::floor(value) != value)
        throw std::runtime_error("The saved Live2D timeline binding is invalid.");
    return static_cast<std::int32_t>(value);
}
bool populated(const std::vector<std::string>& paths) {
    return std::any_of(paths.begin(), paths.end(), [](const auto& path) { return !path.empty(); });
}
PF_Err selectModelMotion(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                        const PF_UserChangedParamExtra* extra) {
    if (!extra || extra->param_index != kOpenDialog) return PF_Err_NONE;
    if (!params || !params[kSelection] || !params[kExpressionSelection]) return PF_Err_BAD_CALLBACK_PARAM;
    TimelineImportScope importing(in_data->pica_basicP);
    const auto original = readSelection(in_data, params[kSelection]->u.arb_d.value);
    const auto expressions = readExpression(in_data, params[kExpressionSelection]->u.arb_d.value);
    ModelSelection pending = dialogSelection(*original, *expressions);
    if (!showModelImportDialog(pending, importing.ownerWindow())) return PF_Err_NONE;
    std::string model; storePath(pending.modelPath, model);
    if (model.empty()) throw std::runtime_error("Choose a Live2D model first.");
    if (model == original->model) return PF_Err_NONE;
    if (populated(original->motions) || populated(expressions->paths) ||
        currentBinding(params[kTimelineBinding]) || currentBinding(params[kExpressionBinding]))
        throw std::runtime_error("This layer already contains animation clips. Create a new layer to import another model.");
    SelectionData changed = *original; changed.model = std::move(model);
    (void)wireSize(changed);
    auto snapshot = std::make_shared<const SelectionData>(std::move(changed));
    ParameterWriteLock<SelectionData> modelLock(in_data, params[kSelection]->u.arb_d.value);
    const std::lock_guard<std::mutex> guard(bankCommitMutex);
    modelLock.write(std::move(snapshot));
    params[kSelection]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
    return PF_Err_NONE;
}
PF_Err selectExpressionClip(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                           const PF_UserChangedParamExtra* extra) {
    if (!extra || extra->param_index != kImportExpression) return PF_Err_NONE;
    if (!params || !params[kSelection] || !params[kExpressionSelection] || !params[kImportTransitionFrames])
        return PF_Err_BAD_CALLBACK_PARAM;
    TimelineImportScope importing(in_data->pica_basicP);
    const auto original = readSelection(in_data, params[kSelection]->u.arb_d.value);
    const auto expressions = readExpression(in_data, params[kExpressionSelection]->u.arb_d.value);
    if (original->model.empty()) throw std::runtime_error("Import a Live2D model before importing motions or expressions.");
    const ModelSelection pending = dialogSelection(*original, *expressions);
    AnimationImportSelection selected;
    selected.transitionCurve = lastImportTransitionCurve();
    const double transition = params[kImportTransitionFrames]->u.fs_d.value;
    if (!std::isfinite(transition) || transition < 0 || transition > 100000 || std::floor(transition) != transition)
        throw std::runtime_error("The default import transition must be a whole number of frames.");
    selected.transitionFrames = static_cast<int>(transition);
    RenderRequest preview; readAmbient(params, preview);
    preview.loop = params[kLoop] && params[kLoop]->u.bd.value;
    preview.ambientSeconds = in_data->time_scale ? static_cast<double>(in_data->current_time) / in_data->time_scale : 0;
    if (!showAnimationImportDialog(pending, selected, importing.ownerWindow(), &preview)) return PF_Err_NONE;
    if (!selected.hasMotion && !selected.hasExpression) throw std::runtime_error("Choose a motion or expression to import.");
    if (selected.transitionFrames < 0 || selected.transitionFrames > 100000)
        throw std::runtime_error("The default import transition is invalid.");
    if (selected.transitionCurve < 0 || selected.transitionCurve > 5)
        throw std::runtime_error("Invalid motion transition curve.");
    SelectionData changed; storePath(selected.selection.modelPath, changed.model);
    if (changed.model != original->model) throw std::runtime_error("The animation import model changed unexpectedly.");
    for (const auto& path : selected.selection.motionPaths) { std::string encoded; storePath(path, encoded); changed.motions.push_back(std::move(encoded)); }
    ExpressionData changedExpressions;
    for (const auto& path : selected.selection.expressionPaths) { std::string encoded; storePath(path, encoded); changedExpressions.paths.push_back(std::move(encoded)); }
    // Bank positions are stable references used by previously created clips.
    const auto preserves = [](const auto& oldPaths, const auto& newPaths) {
        for (size_t i = 0; i < oldPaths.size(); ++i)
            if (!oldPaths[i].empty() && (i >= newPaths.size() || oldPaths[i] != newPaths[i])) return false;
        return true;
    };
    if (!preserves(original->motions, changed.motions) || !preserves(expressions->paths, changedExpressions.paths))
        throw std::runtime_error("Animation import cannot replace an existing clip's saved source.");
    (void)wireSize(changed); (void)wireSize(changedExpressions);
    const bool modelChanged = original->model != changed.model || original->motions != changed.motions;
    const bool expressionsChanged = expressions->paths != changedExpressions.paths;
    auto modelSnapshot = std::make_shared<const SelectionData>(std::move(changed));
    auto expressionSnapshot = std::make_shared<const ExpressionData>(std::move(changedExpressions));
    std::vector<TimelineCommand> commands; commands.reserve(2);
    const auto prepare = [&](const MotionClipSelection& clip, bool expression) {
        const auto& bank = expression ? expressionSnapshot->paths : modelSnapshot->motions;
        if (clip.slot < 1 || static_cast<size_t>(clip.slot) > bank.size() || bank[clip.slot - 1].empty())
            throw std::runtime_error("The selected animation entry is invalid.");
        TimelineCommand command;
        command.oldBinding = currentBinding(params[expression ? kExpressionBinding : kTimelineBinding]);
        command.newBinding = newTimelineBinding(command.oldBinding);
        command.slot = clip.slot; command.duration = clip.duration; command.label = clip.label;
        command.append = clip.append; command.expression = expression; command.transitionFrames = selected.transitionFrames;
        command.transitionCurve = expression ? 0 : selected.transitionCurve;
        commands.push_back(std::move(command));
    };
    if (selected.hasMotion) prepare(selected.motion, false);
    if (selected.hasExpression) prepare(selected.expression, true);
    // Every allocation, encoding and host lock happens before enqueue. The batch
    // queue either accepts both commands or neither; swaps and flag updates cannot fail.
    ParameterWriteLock<SelectionData> modelLock(in_data, params[kSelection]->u.arb_d.value);
    ParameterWriteLock<ExpressionData> expressionLock(in_data, params[kExpressionSelection]->u.arb_d.value);
    enqueueTimelineCommands(in_data->pica_basicP, commands);
    {
        const std::lock_guard<std::mutex> guard(bankCommitMutex);
        if (modelChanged) {
            modelLock.write(std::move(modelSnapshot)); params[kSelection]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
        }
        if (expressionsChanged) {
            expressionLock.write(std::move(expressionSnapshot)); params[kExpressionSelection]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
        }
    }
    for (const auto& command : commands) {
        auto* param = params[command.expression ? kExpressionBinding : kTimelineBinding];
        param->u.fs_d.value = command.newBinding; param->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    }
    if (transition != selected.transitionFrames) {
        params[kImportTransitionFrames]->u.fs_d.value = static_cast<float>(selected.transitionFrames);
        params[kImportTransitionFrames]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    }
    if (selected.hasMotion) (void)rememberImportTransitionCurve(selected.transitionCurve);
    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
    return PF_Err_NONE;
}

PF_Err readLipAudio(PF_InData* in_data, PF_ParamDef* params[], RenderRequest& request) {
    if (!params[kLipSync] || !params[kLipSensitivity]) return PF_Err_BAD_CALLBACK_PARAM;
    if (!params[kLipSync]->u.bd.value) return PF_Err_NONE;
    if (!in_data->inter.checkout_layer_audio || !in_data->inter.get_audio_data ||
        !in_data->inter.checkin_layer_audio) return PF_Err_INVALID_CALLBACK;
    if (!in_data->time_scale) return PF_Err_BAD_CALLBACK_PARAM;
    const double sensitivity = params[kLipSensitivity]->u.fs_d.value;
    if (!std::isfinite(sensitivity)) return PF_Err_BAD_CALLBACK_PARAM;

    // The checkout clock is the effect's layer time, expressed in a finer time
    // scale to keep a 160ms window even at low comp frame rates. Negative layer
    // times are valid. No stateful accumulation: seeks and reverse renders agree.
    constexpr A_u_long sampleRate = static_cast<A_u_long>(kLipAudioSampleRate);
    constexpr A_long windowFrames = static_cast<A_long>(kLipAudioWindowSeconds * sampleRate + 0.5);
    constexpr PF_UFixed fixedRate = static_cast<PF_UFixed>(sampleRate * 65536u);
    const std::int64_t numerator = static_cast<std::int64_t>(in_data->current_time) * sampleRate;
    std::int64_t end = numerator / in_data->time_scale;
    if (numerator < 0 && numerator % in_data->time_scale) --end;
    const std::int64_t start = end - windowFrames;
    if (start < (std::numeric_limits<A_long>::min)() || end > (std::numeric_limits<A_long>::max)())
        return PF_Err_BAD_CALLBACK_PARAM;

    PF_LayerAudio audio = nullptr;
    PF_Err err = PF_CHECKOUT_LAYER_AUDIO(in_data, kAudioLayer, static_cast<A_long>(start),
        windowFrames, sampleRate, fixedRate, PF_SSS_2, PF_Channels_STEREO, PF_SIGNED_PCM, &audio);
    // Only an actual returned handle represents a checkout to release. Never
    // pass null to get/checkin, including after a failed checkout: a None layer
    // can succeed without creating an audio resource.
    // Do not infer audio availability from PF_LayerDef pixel fields: audio-only
    // layers may have no image. A successful null checkout leaves the existing
    // motion/expression mouth unchanged; an actual silent PCM buffer closes it.
    if (!audio) return err;
    try {
        if (!err && audio) {
            PF_SndSamplePtr data = nullptr;
            A_long frames = 0, bytesPerSample = 0, channels = 0, format = 0;
            PF_UFixed actualRate = 0;
            err = PF_GET_AUDIO_DATA(in_data, audio, &data, &frames, &actualRate,
                &bytesPerSample, &channels, &format);
            if (!err) {
                if (frames < 0 || frames > windowFrames + 1 || actualRate != fixedRate ||
                    bytesPerSample != PF_SSS_2 || channels != PF_Channels_STEREO ||
                    format != PF_SIGNED_PCM || (frames > 0 && !data) ||
                    (data && reinterpret_cast<std::uintptr_t>(data) % alignof(std::int16_t))) {
                    err = PF_Err_BAD_CALLBACK_PARAM;
                } else {
                    // SDK_Backwards documents an optional extra terminal zero
                    // frame. Keep it out of the analysis window if supplied.
                    request.mouthOpen = analyzeMouthOpen(static_cast<const std::int16_t*>(data),
                        static_cast<size_t>((std::min)(frames, windowFrames)), channels,
                        sampleRate, std::clamp(sensitivity / 100.0, 0.0, 5.0));
                }
            }
        }
    } catch (...) {
        if (audio) (void)PF_CHECKIN_LAYER_AUDIO(in_data, audio);
        throw;
    }
    const PF_Err cleanupError = audio ? PF_CHECKIN_LAYER_AUDIO(in_data, audio) : PF_Err_NONE;
    if (!err && !cleanupError) request.lipSyncEnabled = true;
    return err ? err : cleanupError;
}

PF_Err renderFrame(PF_InData* in_data, PF_ParamDef* params[], PF_LayerDef* output) {
    if (!output || !params) return PF_Err_BAD_CALLBACK_PARAM;
    if (output->width <= 0 || output->height <= 0) return PF_Err_NONE;
    if (!output->data) return PF_Err_BAD_CALLBACK_PARAM;
    PF_Err err = PF_ABORT(in_data);
    if (err) return err;
    const bool deep = PF_WORLD_IS_DEEP(output);
    const size_t pixelSize = deep ? sizeof(PF_Pixel16) : sizeof(PF_Pixel8);
    if (std::abs(static_cast<long long>(output->rowbytes)) <
        static_cast<long long>(output->width) * static_cast<long long>(pixelSize))
        return PF_Err_BAD_CALLBACK_PARAM;
    std::shared_ptr<const SelectionData> state;
    std::shared_ptr<const ExpressionData> expressions;
    {
        const std::lock_guard<std::mutex> guard(bankCommitMutex);
        state = readSelection(in_data, params[kSelection]->u.arb_d.value);
        expressions = readExpression(in_data, params[kExpressionSelection]->u.arb_d.value);
    }
    if (state->model.empty()) {
        for (A_long y = 0; y != output->height; ++y) {
            auto* row = reinterpret_cast<unsigned char*>(output->data) + static_cast<ptrdiff_t>(y) * output->rowbytes;
            std::memset(row, 0, static_cast<size_t>(output->width) * pixelSize);
        }
        return PF_Err_NONE;
    }

    RenderRequest request;
    request.modelPath = widePath(state->model);
    const auto slotPath = [&](ParameterIndex current, ParameterIndex legacy, const auto& bank) {
        const double index = params[current] ? params[current]->u.fs_d.value : 0;
        if (!std::isfinite(index) || index < 0 || index > kMaximumIndex || std::floor(index) != index)
            throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
        const auto slot = index > 0 ? static_cast<size_t>(index) : static_cast<size_t>(params[legacy]->u.pd.value);
        if (slot < 1 || (index == 0 && slot > kLegacySlots)) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
        if (slot > bank.size()) {
            if (index > 0) throw static_cast<PF_Err>(PF_Err_BAD_CALLBACK_PARAM);
            return std::wstring{}; // An empty legacy slot selects the static pose.
        }
        return widePath(bank[slot - 1]);
    };
    request.motionPath = slotPath(kMotionAIndex, kMotionA, state->motions);
    request.motionPathB = slotPath(kMotionBIndex, kMotionB, state->motions);
    const double time = in_data->time_scale ? static_cast<double>(in_data->current_time) / in_data->time_scale : 0.0;
    readAmbient(params, request);
    // Ambient motion follows the host's layer clock, not either clip's local
    // source time or the legacy Playback speed / Start controls.
    request.ambientSeconds = time;
    const bool manualTime = params[kManualTime]->u.bd.value != 0;
    const double automaticTime = (time - params[kStartTime]->u.fs_d.value) * params[kSpeed]->u.fs_d.value;
    const double timeA = manualTime ? params[kMotionTimeA]->u.fs_d.value : automaticTime;
    const double timeB = manualTime ? params[kMotionTimeB]->u.fs_d.value : automaticTime;
    const double blendPercent = params[kBlend]->u.fs_d.value;
    if (!std::isfinite(timeA) || !std::isfinite(timeB) || !std::isfinite(blendPercent))
        return PF_Err_BAD_CALLBACK_PARAM;
    request.seconds = (std::max)(0.0, timeA);
    request.secondsB = (std::max)(0.0, timeB);
    request.blend = static_cast<float>(std::clamp(blendPercent / 100.0, 0.0, 1.1));
    const double expressionA = params[kExpressionWeightA]->u.fs_d.value;
    const double expressionB = params[kExpressionWeightB]->u.fs_d.value;
    if (!std::isfinite(expressionA) || !std::isfinite(expressionB)) return PF_Err_BAD_CALLBACK_PARAM;
    request.expressionWeightA = static_cast<float>(std::clamp(expressionA / 100.0, 0.0, 1.0));
    request.expressionWeightB = static_cast<float>(std::clamp(expressionB / 100.0, 0.0, 1.0));
    // An inactive expression must not resolve a slot or access a missing asset.
    // In particular, old projects with an empty expression bank render unchanged.
    if (request.expressionWeightA > 0 || request.expressionWeightB > 0) {
        if (request.expressionWeightA > 0) request.expressionPathA = slotPath(kExpressionAIndex, kExpressionA, expressions->paths);
        if (request.expressionWeightB > 0) request.expressionPathB = slotPath(kExpressionBIndex, kExpressionB, expressions->paths);
    }
    request.loop = params[kLoop]->u.bd.value != 0;
    request.width = output->width;
    request.height = output->height;
    request.scale = static_cast<float>(params[kScale]->u.fs_d.value / 100.0);
    const float downX = in_data->downsample_x.den ? static_cast<float>(in_data->downsample_x.num) / in_data->downsample_x.den : 1.0f;
    const float downY = in_data->downsample_y.den ? static_cast<float>(in_data->downsample_y.num) / in_data->downsample_y.den : 1.0f;
    const float hostPixelAspect = in_data->pixel_aspect_ratio.den
        ? static_cast<float>(in_data->pixel_aspect_ratio.num) / in_data->pixel_aspect_ratio.den : 1.0f;
    request.pixelAspect = downX > 0 ? hostPixelAspect * downY / downX : hostPixelAspect;
    request.offsetX = static_cast<float>(params[kOffsetX]->u.fs_d.value) * downX;
    request.offsetY = static_cast<float>(params[kOffsetY]->u.fs_d.value) * downY;
    if (!std::isfinite(request.scale) || !std::isfinite(request.pixelAspect) ||
        !std::isfinite(request.offsetX) || !std::isfinite(request.offsetY)) return PF_Err_BAD_CALLBACK_PARAM;

    if ((err = readLipAudio(in_data, params, request))) return err;
    if ((err = PF_ABORT(in_data))) return err;
    const auto frame = l2dae::render(request);
    if (frame.width != output->width || frame.height != output->height ||
        frame.rgba.size() != static_cast<size_t>(output->width) * output->height * 4)
        throw std::runtime_error("Live2D renderer returned an invalid frame size.");
    if ((err = PF_ABORT(in_data))) return err;
    for (A_long y = 0; y != output->height; ++y) {
        if ((y & 63) == 0 && (err = PF_ABORT(in_data))) return err;
        const auto* source = frame.rgba.data() + static_cast<size_t>(y) * output->width * 4;
        auto* destination = reinterpret_cast<unsigned char*>(output->data) + static_cast<ptrdiff_t>(y) * output->rowbytes;
        if (deep) {
            auto* pixels = reinterpret_cast<PF_Pixel16*>(destination);
            const auto expand = [](std::uint8_t value) -> A_u_short {
                return static_cast<A_u_short>((static_cast<unsigned>(value) * 32768u + 127u) / 255u);
            };
            for (A_long x = 0; x != output->width; ++x, source += 4) {
                pixels[x].red = expand(source[0]);
                pixels[x].green = expand(source[1]);
                pixels[x].blue = expand(source[2]);
                pixels[x].alpha = expand(source[3]);
            }
        } else {
            auto* pixels = reinterpret_cast<PF_Pixel8*>(destination);
            for (A_long x = 0; x != output->width; ++x, source += 4) {
                pixels[x].red = source[0];
                pixels[x].green = source[1];
                pixels[x].blue = source[2];
                pixels[x].alpha = source[3];
            }
        }
    }
    return PF_Err_NONE;
}

PF_Err reportError(PF_OutData* out_data, const char* message, PF_Err error) noexcept {
    if (out_data) {
        try {
            const auto text = std::string("AeGO Flash: ") + errorTextUtf8(message ? message : "");
            const UINT page = gHostCodePage.load(std::memory_order_relaxed);
            copyHostText(out_data->return_msg, sizeof(out_data->return_msg), encodeHostUtf8(text, page, true), page);
        } catch (...) {
            // Pre-encoded fallback is allocation-free, including on a low-memory path.
            const UINT page = gHostCodePage.load(std::memory_order_relaxed);
            const char* fallback = !simplifiedChineseUi() || (page != 936 && page != CP_UTF8)
                ? "AeGO Flash: operation failed."
                : page == 936
                ? "AeGO Flash: \xb2\xd9\xd7\xf7\xca\xa7\xb0\xdc\xa1\xa3"
                : u8"AeGO Flash：操作失败，请重试。";
            std::snprintf(out_data->return_msg, sizeof(out_data->return_msg), "%s", fallback);
        }
        out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
    }
    return error;
}
} // namespace
} // namespace l2dae

// Both discovery generations advertise AE2022's API, not the build SDK's API.
// Product version comes from the host's discovery arguments, not PF_InData.version.
extern "C" DllExport PF_Err PluginDataEntryFunction(PF_PluginDataPtr inPtr,
    PF_PluginDataCB callback, SPBasicSuite* basic, const char*, const char* version) {
    if (!callback) return PF_Err_INVALID_CALLBACK;
    l2dae::initializeHostText(basic, version);
    return callback(inPtr, reinterpret_cast<const A_u_char*>("AeGO Flash"),
        reinterpret_cast<const A_u_char*>("L2DAE Native Renderer"),
        reinterpret_cast<const A_u_char*>("AeGO"), reinterpret_cast<const A_u_char*>("EffectMain"),
        'eFKT', l2dae::kMinimumApiMajor, l2dae::kMinimumApiMinor, AE_RESERVED_INFO);
}
extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 callback, SPBasicSuite* basic, const char*, const char* version) {
    if (!callback) return PF_Err_INVALID_CALLBACK;
    l2dae::initializeHostText(basic, version);
    return callback(inPtr, reinterpret_cast<const A_u_char*>("AeGO Flash"),
        reinterpret_cast<const A_u_char*>("L2DAE Native Renderer"),
        reinterpret_cast<const A_u_char*>("AeGO"), reinterpret_cast<const A_u_char*>("EffectMain"),
        'eFKT', l2dae::kMinimumApiMajor, l2dae::kMinimumApiMinor, AE_RESERVED_INFO,
        reinterpret_cast<const A_u_char*>(""));
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output, void* extra) {
    using namespace l2dae;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            initializeHostText(in_data ? in_data->pica_basicP : nullptr);
            std::snprintf(out_data->return_msg, sizeof(out_data->return_msg),
                "%s", hostText(uiText(
                L"AeGO Flash 1.0.2\rImport a model, then preview motions and expressions.\r"
                L"Breathing and blinking can be adjusted and keyframed.\r"
                L"Choose an audio layer and enable lip sync.\r"
                L"Uses external model files. 8-bit render, for 8- or 16-bit comps.",
                L"AeGO Flash 1.0.2\r导入模型后，可预览并导入动作与表情。\r"
                L"呼吸与眨眼支持参数调节和关键帧。\r"
                L"选择音频图层，启用声音同步口型。\r"
                L"使用外部模型文件；8 位渲染，可输出至 8/16 位合成。")).c_str());
            return PF_Err_NONE;
        case PF_Cmd_GLOBAL_SETUP:
            initializeHostText(in_data ? in_data->pica_basicP : nullptr);
            out_data->my_version = kPluginVersion;
            out_data->out_flags = kOutputFlags;
            out_data->out_flags2 = kOutputFlags2;
            (void)initializeTimelineHost(in_data ? in_data->pica_basicP : nullptr);
            return PF_Err_NONE;
        case PF_Cmd_GLOBAL_SETDOWN:
            // AE guarantees global selectors never overlap any other selector,
            // including MFR renders. Renderer teardown also waits for its workers.
            shutdownTimelineHost();
            releaseRenderer();
            return PF_Err_NONE;
        case PF_Cmd_PARAMS_SETUP:
            initializeHostText(in_data ? in_data->pica_basicP : nullptr);
            return setupParameters(in_data, out_data);
        case PF_Cmd_USER_CHANGED_PARAM:
            if (extra && static_cast<PF_UserChangedParamExtra*>(extra)->param_index == kImportExpression)
                return selectExpressionClip(in_data, out_data, params, static_cast<PF_UserChangedParamExtra*>(extra));
            return selectModelMotion(in_data, out_data, params, static_cast<PF_UserChangedParamExtra*>(extra));
        case PF_Cmd_ARBITRARY_CALLBACK:
            return arbitraryCallback(in_data, static_cast<PF_ArbParamsExtra*>(extra));
        case PF_Cmd_RENDER:
            return renderFrame(in_data, params, output);
        default:
            return PF_Err_NONE;
        }
    } catch (const PF_Err& err) {
        if (err == PF_Interrupt_CANCEL || err == PF_Err_NONE) return err;
        return reportError(out_data, err == PF_Err_OUT_OF_MEMORY ? "Not enough memory." :
            u8"AE 无法完成本次操作。请检查素材与效果参数后重试。", err);
    } catch (const std::bad_alloc&) {
        return reportError(out_data, "Not enough memory.", PF_Err_OUT_OF_MEMORY);
    } catch (const std::exception& e) {
        return reportError(out_data, e.what(), PF_Err_BAD_CALLBACK_PARAM);
    } catch (...) {
        return reportError(out_data, "Unexpected native rendering error.", PF_Err_INTERNAL_STRUCT_DAMAGED);
    }
}

