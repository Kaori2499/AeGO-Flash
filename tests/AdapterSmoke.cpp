// Exercises the actual AEX binary using official AE 26.5 callback structures.
// This small fake host does not replace validation inside After Effects.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_PluginData.h"
#include "AE_GeneralPlug.h"
#include "SPBasic.h"
#include "../src/TimelineHost.h"
#include "../src/Plugin.h"
#include "AE_EffectUI.h"
#include "AE_EffectSuites.h"
using namespace l2dae;
#include <array>
#include <vector>
#include <unordered_map>
#include <string>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

struct Allocation { size_t size; unsigned locks = 0; };
static std::unordered_map<PF_Handle, Allocation> allocations;
static std::mutex allocationsMutex;
static std::vector<PF_ParamDef> parameters(1);
static PF_ParamDef importTransitionRegistration{};
static bool capturedImportTransitionRegistration = false;
static std::atomic<unsigned> checks{0};
static bool failHostAllocation = false;
static void require(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
    ++checks;
}
static PF_Handle allocate(A_u_longlong size) {
    if (failHostAllocation) throw std::bad_alloc();
    auto** h = new char*(new char[static_cast<size_t>(size)]{});
    auto handle = reinterpret_cast<PF_Handle>(h);
    const std::lock_guard<std::mutex> guard(allocationsMutex);
    allocations.emplace(handle, Allocation{static_cast<size_t>(size)});
    return handle;
}
static void* lock(PF_Handle handle) {
    const std::lock_guard<std::mutex> guard(allocationsMutex);
    ++allocations.at(handle).locks;
    return *handle;
}
static void unlock(PF_Handle handle) {
    const std::lock_guard<std::mutex> guard(allocationsMutex);
    auto& value = allocations.at(handle);
    require(value.locks != 0, "balanced host handle unlock");
    --value.locks;
}
static void dispose(PF_Handle handle) {
    const std::lock_guard<std::mutex> guard(allocationsMutex);
    require(allocations.at(handle).locks == 0, "host handles are unlocked before disposal");
    require(allocations.erase(handle) == 1, "double-disposal or foreign handle");
    delete[] reinterpret_cast<char*>(*handle);
    delete reinterpret_cast<char**>(handle);
}
static A_u_longlong sizeOf(PF_Handle handle) {
    const std::lock_guard<std::mutex> guard(allocationsMutex);
    return allocations.at(handle).size;
}
static PF_Err addParam(PF_ProgPtr, PF_ParamIndex index, PF_ParamDefPtr def) {
    require(index == -1, "parameters append");
    if (def->uu.id == 133) {
        importTransitionRegistration = *def;
        capturedImportTransitionRegistration = true;
    }
    parameters.push_back(*def);
    if (def->param_type == PF_Param_ARBITRARY_DATA)
        parameters.back().u.arb_d.value = def->u.arb_d.dephault;
    // Emulate a newly applied effect. With USE_VALUE_FOR_OLD_PROJECTS, value
    // is specifically the missing-parameter fallback for old saved projects;
    // new effects and Reset use dephault. Retain the raw registration above
    // so both contracts can be asserted independently of this host behavior.
    if (def->param_type == PF_Param_FLOAT_SLIDER &&
        (def->flags & PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS))
        parameters.back().u.fs_d.value = def->u.fs_d.dephault;
    return PF_Err_NONE;
}
static thread_local PF_Err abortError = PF_Err_NONE;
static PF_Err abortNone(PF_ProgPtr) { return abortError; }
static A_Err registration(PF_PluginDataPtr, const A_u_char* name,
    const A_u_char* match, const A_u_char* category, const A_u_char* entry,
    A_long kind, A_long major, A_long minor, A_long reserved, const A_u_char* url) {
    require(std::strcmp(reinterpret_cast<const char*>(name), "AeGO Flash") == 0, "registered display name");
    require(std::strcmp(reinterpret_cast<const char*>(match), "L2DAE Native Renderer") == 0, "registered match name");
    require(std::strcmp(reinterpret_cast<const char*>(category), "AeGO") == 0, "registered category");
    require(std::strcmp(reinterpret_cast<const char*>(entry), "EffectMain") == 0, "registered entry point");
    require(kind == 0x65464b54 && major == 13 && minor == 27, "registered API");
    require(reserved == 8 && url && !*url, "registered optional support data");
    return A_Err_NONE;
}
static A_Err legacyRegistration(PF_PluginDataPtr data, const A_u_char* name,
    const A_u_char* match, const A_u_char* category, const A_u_char* entry,
    A_long kind, A_long major, A_long minor, A_long reserved) {
    return registration(data, name, match, category, entry, kind, major, minor, reserved,
        reinterpret_cast<const A_u_char*>(""));
}
using Effect = PF_Err(*)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef**, PF_LayerDef*, void*);
namespace encodingMock {
static int references = 0;
static bool failAcquire = false, failLanguage = false;
static unsigned failedAcquires = 0, failedLanguageCalls = 0;
static PF_Err language(A_char* text) {
    std::memcpy(text, failLanguage ? "en_US" : "zh_CN", 6);
    if (failLanguage) { ++failedLanguageCalls; return PF_Err_INVALID_CALLBACK; }
    return PF_Err_NONE;
}
static PFAppSuite6 appSuite = [] { PFAppSuite6 suite{}; suite.PF_AppGetLanguage = language; return suite; }();
static SPErr acquire(const char* name, int32 version, const void** suite) {
    *suite = nullptr;
    if (std::strcmp(name, kPFAppSuite) || version != kPFAppSuiteVersion6) return 1;
    if (failAcquire) { ++failedAcquires; return 1; }
    *suite = &appSuite; ++references; return 0;
}
static SPErr release(const char* name, int32 version) {
    require(!std::strcmp(name, kPFAppSuite) && version == kPFAppSuiteVersion6 && references > 0,
        "encoding probes balance language suite references");
    --references; return 0;
}
static std::wstring decode(const char* text, UINT page) {
    const int count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    require(count > 0, "actual AEX UI bytes decode strictly in the selected host encoding");
    std::wstring result(static_cast<size_t>(count), L'\0');
    require(MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text, -1, result.data(), count) == count,
        "decode actual AEX label without replacement");
    result.pop_back(); return result;
}
static void run(Effect effect, PluginDataEntryFunctionPtr legacy, PluginDataEntryFunction2Ptr modern,
                const PF_InData& baseInput) {
    SPBasicSuite basic{}; basic.AcquireSuite = acquire; basic.ReleaseSuite = release;
    PF_InData in = baseInput; in.pica_basicP = &basic;
    struct Expected { ParameterIndex index; const wchar_t* text; };
    const Expected labels[] = {
        {kOpenDialog,L"导入模型"}, {kSelection,L"模型选择"}, {kLoop,L"循环动作"},
        {kSpeed,L"播放速度"}, {kStartTime,L"起始时间 (秒)"}, {kScale,L"缩放 (%)"},
        {kOffsetX,L"水平偏移 (像素)"}, {kOffsetY,L"垂直偏移 (像素)"},
        {kMotionA,L"动作 A 槽位"}, {kMotionB,L"动作 B 槽位"}, {kManualTime,L"动作时间关键帧"},
        {kMotionTimeA,L"动作 A 时间 (秒)"}, {kMotionTimeB,L"动作 B 时间 (秒)"},
        {kBlend,L"动作 A 到 B 过渡 (%)"}, {kTimelineBinding,L"动作轨道绑定"},
        {kImportExpression,L"导入动作与表情"}, {kExpressionSelection,L"表情选择"},
        {kExpressionA,L"表情 A 槽位"}, {kExpressionB,L"表情 B 槽位"},
        {kExpressionWeightA,L"表情 A 强度 (%)"}, {kExpressionWeightB,L"表情 B 强度 (%)"},
        {kExpressionBinding,L"表情轨道绑定"}, {kAudioLayer,L"音频图层"},
        {kLipSync,L"声音同步口型"}, {kLipSensitivity,L"口型灵敏度 (%)"},
        {kBreathing,L"呼吸动画"}, {kAutoBlink,L"自动眨眼"}, {kBreathAmount,L"呼吸幅度 (%)"},
        {kBreathPeriod,L"呼吸周期 (秒)"}, {kBlinkStrength,L"眨眼强度 (%)"},
        {kBlinkInterval,L"眨眼间隔 (秒)"}, {kBlinkDuration,L"眨眼时长 (秒)"},
        {kImportTransitionFrames,L"默认过渡 (帧)"}, {kMotionAIndex,L"动作 A 编号"},
        {kMotionBIndex,L"动作 B 编号"}, {kExpressionAIndex,L"表情 A 编号"}, {kExpressionBIndex,L"表情 B 编号"},
        {kModelGroupStart,L"模型与动画"}, {kLayoutGroupStart,L"位置与大小"},
        {kAudioGroupStart,L"声音与口型"}, {kAmbientGroupStart,L"呼吸与眨眼"}};
    struct Retained { const char* text; std::string bytes; };
    std::vector<Retained> retained;
    struct Case { const char* version; bool oldEntry; bool legacyEncoding; };
    for (const auto& test : std::array<Case, 8>{{
            {"22.0.0", true, true}, {"26.3x87", false, false}, {"22.6", false, true},
            {"24.0", false, true}, {"25.3", true, true}, {"25.6", false, true},
            {"26.3x87", true, false}, {"unknown-host", false, false}}}) {
        failAcquire = failLanguage = false;
        require((test.oldEntry ? legacy(nullptr, legacyRegistration, &basic, "After Effects", test.version) :
            modern(nullptr, registration, &basic, "After Effects", test.version)) == 0,
            "both actual DLL discovery entries accept AE22/24/25 legacy text and AE26 UTF-8 hosts");
        const UINT page = test.legacyEncoding && GetOEMCP() != CP_UTF8 ? 936 : CP_UTF8;
        PF_OutData out{};
        require(effect(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) == 0, "encoding probe global setup");
        require(parameters.size() == 1 && allocations.empty(), "encoding probe starts with no old host-owned handles");
        failHostAllocation = true;
        const auto memoryError = effect(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr);
        failHostAllocation = false;
        require(memoryError == PF_Err_OUT_OF_MEMORY && (out.out_flags & PF_OutFlag_DISPLAY_ERROR_MESSAGE),
            "actual DLL localized error is returned through the host error field");
        require(decode(out.return_msg, page) == L"AeGO Flash: 可用内存不足。请降低合成尺寸或释放内存后重试。",
            "Chinese error message survives the selected host encoding exactly");
        require(allocations.empty(), "failed setup does not leak host allocations");
        const auto verifyUi = [&] {
            parameters.assign(1, PF_ParamDef{});
            require(effect(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) == 0 && parameters.size() == kParameterCount,
                "encoding probe creates every real parameter and group");
            for (const auto& label : labels) {
                require(decode(parameters[label.index].PF_DEF_NAME, page) == label.text, "all saved and grouped labels decode exactly");
                require(std::strlen(parameters[label.index].PF_DEF_NAME) < 32, "host parameter name fits without multibyte truncation");
            }
            const auto pointer = [&](const char* text, const wchar_t* expected) {
                require(text && decode(text, page) == expected, "persistent button/checkbox/popup text is valid after setup returns");
                retained.push_back({text, text});
            };
            pointer(parameters[kOpenDialog].u.button_d.u.namesptr, L"导入模型…");
            pointer(parameters[kImportExpression].u.button_d.u.namesptr, L"导入动作与表情…");
            for (auto index : {kLoop, kManualTime, kLipSync, kBreathing, kAutoBlink})
                pointer(parameters[index].u.bd.u.nameptr, L"启用");
            for (auto index : {kMotionA, kMotionB, kExpressionA, kExpressionB})
                pointer(parameters[index].u.pd.u.namesptr, L"槽位 1|槽位 2|槽位 3|槽位 4|槽位 5|槽位 6|槽位 7|槽位 8");
            require(effect(PF_Cmd_ABOUT, &in, &out, nullptr, nullptr, nullptr) == 0, "localized About callback");
            const auto about = decode(out.return_msg, page);
            require(about.find(L"AeGO Flash 1.0.1") == 0 && about.find(L"选择音频图层，启用声音同步口型。") != std::wstring::npos,
                "About body decodes correctly with the public version");
            for (auto index : {kSelection, kExpressionSelection}) {
                PF_ArbParamsExtra extra{}; extra.id = static_cast<A_short>(parameters[index].uu.id);
                extra.which_function = PF_Arbitrary_DISPOSE_FUNC;
                extra.u.dispose_func_params.arbH = parameters[index].u.arb_d.dephault;
                require(effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0, "encoding probe releases default bank");
            }
            parameters.assign(1, PF_ParamDef{});
            require(allocations.empty() && references == 0, "every UI snapshot balances optional suites and host allocations");
        };
        verifyUi();
        if (std::strcmp(test.version, "unknown-host")) {
            require((test.oldEntry ? legacy(nullptr, legacyRegistration, &basic, "After Effects", "not-a-version") :
                modern(nullptr, registration, &basic, "After Effects", "not-a-version")) == 0,
                "malformed registration hints are non-fatal after a valid host version");
            verifyUi(); // A bad hint must not replace the known encoding context.
            const auto acquireFailuresBefore = failedAcquires;
            failAcquire = true;
            verifyUi();
            failAcquire = false;
            require(failedAcquires >= acquireFailuresBefore + 2 && references == 0,
                "About and Params preserve known Chinese context when language-suite acquisition fails");
            const auto languageFailuresBefore = failedLanguageCalls;
            failLanguage = true;
            verifyUi();
            failLanguage = false;
            require(failedLanguageCalls >= languageFailuresBefore + 2 && references == 0,
                "failing language callbacks cannot replace known context with their en_US output and release all suites");
        }
        require(effect(PF_Cmd_GLOBAL_SETDOWN, &in, &out, nullptr, nullptr, nullptr) == 0, "encoding probe global teardown");
        parameters.assign(1, PF_ParamDef{});
        require(allocations.empty() && references == 0, "encoding probe balances language suites and host allocations");
        for (const auto& saved : retained)
            require(std::strcmp(saved.text, saved.bytes.c_str()) == 0, "registering another host encoding does not invalidate retained UI text pointers");
    }
    std::cout << "PASS: actual DLL legacy/modern discovery; AE22/24/25 Chinese and AE26/unknown UTF-8 labels, button/checkbox/popup text, About and error bytes; malformed hints and optional language failures preserve established context; retained pointer lifetimes.\n";
}
}
namespace audioMock {
constexpr A_long rate = 22050, frameCount = 3528;
constexpr PF_UFixed fixedRate = static_cast<PF_UFixed>(rate * 65536u);
thread_local bool provide = false, live = false, nullData = false, misaligned = false;
thread_local PF_Err checkoutError = 0, getError = 0, checkinError = 0;
thread_local unsigned checkouts = 0, handles = 0, gets = 0, checkins = 0;
thread_local A_long returnedFrames = frameCount, returnedBytes = PF_SSS_2;
thread_local A_long returnedChannels = PF_Channels_STEREO, returnedFormat = PF_SIGNED_PCM;
thread_local PF_UFixed returnedRate = fixedRate;
thread_local A_long lastStart = 0, lastDuration = 0;
thread_local A_u_long lastScale = 0;
thread_local std::vector<std::int16_t> samples((frameCount + 1) * 2);
thread_local unsigned char tokenStorage = 0;
thread_local const PF_LayerAudio token = reinterpret_cast<PF_LayerAudio>(&tokenStorage);
void reset() {
    require(!live, "audio mock resets without leaked checkout");
    provide = false; nullData = false; misaligned = false;
    checkoutError = getError = checkinError = 0;
    returnedFrames = frameCount; returnedBytes = PF_SSS_2;
    returnedChannels = PF_Channels_STEREO; returnedFormat = PF_SIGNED_PCM; returnedRate = fixedRate;
    std::fill(samples.begin(), samples.end(), std::int16_t{});
}
void tone(double amplitude) {
    for (A_long i = 0; i < frameCount; ++i) {
        const auto value = static_cast<std::int16_t>(std::round(amplitude * 32767.0 *
            std::sin(2.0 * 3.141592653589793 * 220.0 * i / rate)));
        samples[static_cast<size_t>(i) * 2] = value;
        samples[static_cast<size_t>(i) * 2 + 1] = static_cast<std::int16_t>(-value);
    }
}
PF_Err checkout(PF_ProgPtr, PF_ParamIndex index, A_long start, A_long duration,
    A_u_long scale, PF_UFixed sampleRate, A_long bytes, A_long channels, A_long format,
    PF_LayerAudio* audio) {
    require(!live && audio && index == kAudioLayer, "audio checkout uses layer parameter, not source pixels");
    require(duration == frameCount && scale == rate && sampleRate == fixedRate &&
        bytes == PF_SSS_2 && channels == PF_Channels_STEREO && format == PF_SIGNED_PCM,
        "requests exactly 160 ms signed PCM16 stereo at 22050 Hz");
    ++checkouts; lastStart = start; lastDuration = duration; lastScale = scale;
    *audio = provide ? token : nullptr;
    live = provide;
    if (live) ++handles;
    return checkoutError;
}
PF_Err get(PF_ProgPtr, PF_LayerAudio audio, PF_SndSamplePtr* data, A_long* frames,
    PF_UFixed* sampleRate, A_long* bytes, A_long* channels, A_long* format) {
    require(audio == token && live, "get audio data only for a live nonnull checkout");
    ++gets;
    *data = nullData ? nullptr : reinterpret_cast<char*>(samples.data()) + (misaligned ? 1 : 0);
    *frames = returnedFrames; *sampleRate = returnedRate; *bytes = returnedBytes;
    *channels = returnedChannels; *format = returnedFormat;
    return getError;
}
PF_Err checkin(PF_ProgPtr, PF_LayerAudio audio) {
    require(audio && audio == token && live,
        "REGRESSION: checkin must receive an actual live audio handle, never null");
    ++checkins; live = false;
    return checkinError;
}
void install(PF_InData& in) {
    in.inter.checkout_layer_audio = checkout;
    in.inter.get_audio_data = get;
    in.inter.checkin_layer_audio = checkin;
}
}
static std::string hexText(const std::string& text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char value : text) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}
static std::string utf8Path(const std::wstring& path) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    require(count > 0, "valid model/motion path");
    std::string result(static_cast<size_t>(count), '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), result.data(), count, nullptr, nullptr) == count, "encode model/motion path");
    return result;
}

static std::atomic<unsigned> renderSuiteCalls{0};
static SPErr rejectRenderSuite(const char*, int32, const void** result) {
    ++renderSuiteCalls;
    if (result) *result = nullptr;
    return 1;
}

// Exercise the DLL concurrently with AE-like evaluated parameter snapshots.
// Immutable banks may be shared; audio state, PF_InData/OutData and pixels may not.
static void runConcurrentRenders(Effect effect, const PF_InData& input,
                                const std::vector<PF_ParamDef>& sourceParameters) {
    constexpr unsigned caseCount = 16;
    const auto renderCase = [&](unsigned index, bool cancelAudio = false) {
        auto values = sourceParameters;
        PF_InData in = input;
        PF_OutData out{};
        SPBasicSuite basic{};
        basic.AcquireSuite = rejectRenderSuite;
        in.pica_basicP = &basic;
        in.current_time = static_cast<A_long>(index * 431) - 875;
        in.time_scale = 1000;
        in.downsample_x = {1, 1}; in.downsample_y = {1, 1};
        in.pixel_aspect_ratio.num = index % 3 ? 1 : 4;
        in.pixel_aspect_ratio.den = index % 3 ? 1 : 3;
        values[kManualTime].u.bd.value = TRUE;
        values[kMotionTimeA].u.fs_d.value = 0.25 + index * 0.11;
        values[kMotionTimeB].u.fs_d.value = 1.75 - index * 0.08;
        values[kBlend].u.fs_d.value = index * 100.0 / (caseCount - 1);
        values[kExpressionWeightA].u.fs_d.value = (index % 4) * 25;
        values[kExpressionWeightB].u.fs_d.value = ((index + 1) % 4) * 25;
        values[kLipSync].u.bd.value = cancelAudio || index % 4 != 0;
        values[kLipSensitivity].u.fs_d.value = 75 + index * 5;
        values[kBreathing].u.bd.value = (index & 2) != 0;
        values[kAutoBlink].u.bd.value = (index & 4) != 0;
        std::vector<PF_ParamDef*> params;
        for (auto& value : values) params.push_back(&value);
        const auto unchanged = values;
        audioMock::reset();
        audioMock::provide = cancelAudio || index % 5 != 0;
        audioMock::tone((index % 3) * 0.12);
        audioMock::getError = cancelAudio ? PF_Interrupt_CANCEL : PF_Err_NONE;
        audioMock::install(in);
        abortError = PF_Err_NONE;

        for (size_t parameter : {kSelection, kExpressionSelection}) {
            PF_ArbitraryH copied = nullptr;
            PF_ArbParamsExtra extra{};
            extra.id = parameter == kSelection ? 102 : 117;
            extra.which_function = PF_Arbitrary_COPY_FUNC;
            extra.u.copy_func_params.src_arbH = values[parameter].u.arb_d.value;
            extra.u.copy_func_params.dst_arbPH = &copied;
            require(effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0 && copied,
                "concurrent arbitrary copy owns a new handle");
            require(copied != values[parameter].u.arb_d.value,
                "MFR snapshot copies never alias the source handle");
            A_u_long wireSize = 0;
            extra.which_function = PF_Arbitrary_FLAT_SIZE_FUNC;
            extra.u.flat_size_func_params.arbH = copied;
            extra.u.flat_size_func_params.flat_data_sizePLu = &wireSize;
            require(effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0 && wireSize,
                "concurrent variable-size snapshot query succeeds");
            std::vector<unsigned char> copiedBytes(wireSize), originalBytes(wireSize);
            extra.which_function = PF_Arbitrary_FLATTEN_FUNC;
            extra.u.flatten_func_params.arbH = copied;
            extra.u.flatten_func_params.buf_sizeLu = wireSize;
            extra.u.flatten_func_params.flat_dataPV = copiedBytes.data();
            require(effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0,
                "concurrent snapshot flatten succeeds");
            extra.u.flatten_func_params.arbH = values[parameter].u.arb_d.value;
            extra.u.flatten_func_params.flat_dataPV = originalBytes.data();
            require(effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0 &&
                copiedBytes == originalBytes, "concurrent flatten reads an unchanged complete snapshot");
            extra.which_function = PF_Arbitrary_DISPOSE_FUNC;
            extra.u.dispose_func_params.arbH = copied;
            require(effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra) == 0,
                "concurrent copied snapshot disposal balances allocation");
        }

        const bool deep = (index & 1) != 0;
        const size_t pixelBytes = deep ? sizeof(PF_Pixel16) : sizeof(PF_Pixel8);
        const A_long width = static_cast<A_long>(96 + 16 * (index % 3));
        const A_long height = static_cast<A_long>(80 + 16 * (index % 2));
        const size_t rowBytes = static_cast<size_t>(width) * pixelBytes + 12;
        constexpr size_t edgeGuard = 64;
        std::vector<unsigned char> pixels(rowBytes * height + 2 * edgeGuard, 0xcd);
        PF_LayerDef frame{};
        frame.width = width; frame.height = height;
        frame.world_flags = deep ? PF_WorldFlag_DEEP : 0;
        frame.rowbytes = static_cast<A_long>(rowBytes);
        frame.data = reinterpret_cast<PF_PixelPtr>(pixels.data() + edgeGuard);
        if (index & 2) {
            frame.rowbytes = -frame.rowbytes;
            frame.data = reinterpret_cast<PF_PixelPtr>(pixels.data() + edgeGuard + rowBytes * (height - 1));
        }
        const unsigned beforeCheckouts = audioMock::checkouts;
        const unsigned beforeHandles = audioMock::handles, beforeCheckins = audioMock::checkins;
        const auto error = effect(PF_Cmd_RENDER, &in, &out, params.data(), &frame, nullptr);
        if (error != (cancelAudio ? PF_Interrupt_CANCEL : PF_Err_NONE))
            throw std::runtime_error(std::string("concurrent AEX frame failed: ") + out.return_msg);
        require(!audioMock::live && audioMock::handles - beforeHandles == audioMock::checkins - beforeCheckins,
            "concurrent success/cancellation releases only that frame's audio handle");
        require(audioMock::checkouts - beforeCheckouts == (values[kLipSync].u.bd.value ? 1u : 0u),
            "concurrent disabled lip sync makes no checkout");
        require(std::memcmp(values.data(), unchanged.data(), values.size() * sizeof(PF_ParamDef)) == 0,
            "render leaves its evaluated parameter snapshot unchanged");
        require(std::all_of(pixels.begin(), pixels.begin() + edgeGuard, [](unsigned char c) { return c == 0xcd; }) &&
            std::all_of(pixels.end() - edgeGuard, pixels.end(), [](unsigned char c) { return c == 0xcd; }),
            "concurrent 8/16-bpc frames preserve allocation guards");
        for (A_long y = 0; y < height; ++y) {
            const auto first = pixels.begin() + edgeGuard + rowBytes * y + width * pixelBytes;
            require(std::all_of(first, first + 12, [](unsigned char c) { return c == 0xcd; }),
                "concurrent positive/negative rowbytes padding is untouched");
        }
        return pixels;
    };

    std::array<std::vector<unsigned char>, caseCount> serial;
    for (unsigned i = 0; i < caseCount; ++i) serial[i] = renderCase(i);
    for (unsigned threadCount : {4u, 8u}) {
        std::mutex gateMutex;
        std::condition_variable gate;
        unsigned ready = 0;
        bool start = false;
        std::vector<std::exception_ptr> failures(threadCount);
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        const auto worker = [&](unsigned workerIndex) {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                ++ready;
                gate.notify_all();
                gate.wait(lock, [&] { return start; });
            }
            try {
                // Cancel each thread once, then verify that healthy frames on
                // that thread remain unaffected by its own and other errors.
                (void)renderCase(workerIndex, true);
                for (unsigned offset = 0; offset < caseCount; offset += threadCount) {
                    const unsigned index = (workerIndex * 5 + offset) % caseCount;
                    require(renderCase(index) == serial[index],
                        "4/8 simultaneous AEX callers exactly match serial 8/16-bpc motion/expression/audio/ambient pixels");
                }
            } catch (...) { failures[workerIndex] = std::current_exception(); }
        };
        try {
            for (unsigned i = 0; i < threadCount; ++i) threads.emplace_back(worker, i);
        } catch (...) {
            { const std::lock_guard<std::mutex> lock(gateMutex); start = true; }
            gate.notify_all();
            for (auto& thread : threads) thread.join();
            throw;
        }
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            gate.wait(lock, [&] { return ready == threadCount; });
            start = true;
        }
        gate.notify_all();
        for (auto& thread : threads) thread.join();
        for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    }
    require(renderSuiteCalls == 0, "render never acquires AEGP suites on worker threads");
    std::cout << "PASS: actual AEX 4/8-thread concurrent calls; independent evaluated parameters/audio/outputs; shared read-only banks and concurrent copy/flatten/dispose; serial-identical 8/16-bpc pixels; cancellation recovery; no render AEGP access.\n";
}

namespace timelineMock {
AEGP_UtilitySuite6 utility{};
AEGP_RegisterSuite5 registrationSuite{};
AEGP_MemorySuite1 memory{};
SPBasicSuite basic{};
AEGP_IdleHook idle = nullptr;
AEGP_IdleRefcon idleRefcon = nullptr;
AEGP_GlobalRefcon globalRefcon = nullptr;
std::unordered_map<AEGP_MemHandle, std::string> handles;
std::vector<std::string> scripts;
std::vector<std::string> errors;
unsigned suiteReferences = 0, registerCalls = 0, idleRegistrations = 0, locks = 0;
std::uintptr_t nextHandle = 1;
bool suppressUI = false, scriptingAvailable = true;
enum class ResultMode { Success, Error, Unexpected };
ResultMode mode = ResultMode::Success;

SPErr acquire(const char* name, int32 version, const void** result) {
    *result = nullptr;
    if (!std::strcmp(name, kAEGPUtilitySuite) && version == kAEGPUtilitySuiteVersion6) *result = &utility;
    if (!std::strcmp(name, kAEGPRegisterSuite) && version == kAEGPRegisterSuiteVersion5) *result = &registrationSuite;
    if (!std::strcmp(name, kAEGPMemorySuite) && version == kAEGPMemorySuiteVersion1) *result = &memory;
    if (!*result) return 1;
    ++suiteReferences;
    return 0;
}
SPErr release(const char*, int32) { require(suiteReferences > 0, "balanced AEGP suite release"); --suiteReferences; return 0; }
A_Err suppressed(A_Boolean* result) { *result = suppressUI; return 0; }
A_Err available(A_Boolean* result) { *result = scriptingAvailable; return 0; }
A_Err registerPlugin(AEGP_GlobalRefcon refcon, const A_char* name, AEGP_PluginID* id) {
    require(refcon && std::strcmp(name, "AeGO Flash") == 0, "AEGP plugin registration uses the public brand");
    ++registerCalls; globalRefcon = refcon; *id = 51; return 0;
}
A_Err registerIdle(AEGP_PluginID id, AEGP_IdleHook function, AEGP_IdleRefcon refcon) {
    require(id == 51 && function && refcon, "valid idle hook registration");
    ++idleRegistrations; idle = function; idleRefcon = refcon; return 0;
}
A_Err requestIdle() { return 0; } // Mirrors the SDK's asynchronous contract.
A_Err report(AEGP_PluginID id, const A_char* text) {
    require(id == 51 && text, "script failure reported to user"); errors.emplace_back(text); return 0;
}
AEGP_MemHandle makeMemory(std::string text) {
    const auto handle = reinterpret_cast<AEGP_MemHandle>(nextHandle++);
    handles.emplace(handle, std::move(text)); return handle;
}
A_Err memSize(AEGP_MemHandle handle, AEGP_MemSize* size) {
    *size = static_cast<AEGP_MemSize>(handles.at(handle).size() + 1); return 0;
}
A_Err memLock(AEGP_MemHandle handle, void** address) {
    *address = handles.at(handle).data(); ++locks; return 0;
}
A_Err memUnlock(AEGP_MemHandle) { require(locks > 0, "balanced script-handle unlock"); --locks; return 0; }
A_Err memFree(AEGP_MemHandle handle) { require(handles.erase(handle) == 1, "free result/error exactly once"); return 0; }
A_Err execute(AEGP_PluginID id, const A_char* script, A_Boolean platformEncoding,
              AEGP_MemHandle* result, AEGP_MemHandle* error) {
    require(id == 51 && !platformEncoding && script, "UTF-8 script execution contract");
    scripts.emplace_back(script);
    const size_t before = scripts.size();
    A_long sleep = 60;
    require(idle(globalRefcon, idleRefcon, &sleep) == 0 && scripts.size() == before,
        "nested idle cannot replay or drain another pending command");
    *result = makeMemory(mode == ResultMode::Success ? " \r\nOK\n" : mode == ResultMode::Unexpected ? "not OK" : "");
    *error = makeMemory(mode == ResultMode::Error ? "fixture script failure" : "");
    return mode == ResultMode::Error ? 1 : 0;
}
void tick() {
    A_long sleep = 60;
    require(idle && idle(globalRefcon, idleRefcon, &sleep) == 0, "idle callback contains all errors");
    require(handles.empty() && locks == 0 && suiteReferences == 0, "no script handle or AEGP suite leaks");
}
void run() {
    basic.AcquireSuite = acquire; basic.ReleaseSuite = release;
    utility.AEGP_GetSuppressInteractiveUI = suppressed;
    utility.AEGP_IsScriptingAvailable = available;
    utility.AEGP_RegisterWithAEGP = registerPlugin;
    utility.AEGP_ExecuteScript = execute;
    utility.AEGP_ReportInfo = report;
    utility.AEGP_CauseIdleRoutinesToBeCalled = requestIdle;
    registrationSuite.AEGP_RegisterIdleHook = registerIdle;
    memory.AEGP_FreeMemHandle = memFree; memory.AEGP_GetMemHandleSize = memSize;
    memory.AEGP_LockMemHandle = memLock; memory.AEGP_UnlockMemHandle = memUnlock;
    require(!l2dae::initializeTimelineHost(nullptr), "missing AEGP is harmless at setup");
    suppressUI = true;
    require(!l2dae::initializeTimelineHost(&basic), "headless host does not register interactive bridge");
    suppressUI = false;
    require(l2dae::initializeTimelineHost(&basic), "bridge registers against official AEGP suites");
    require(l2dae::initializeTimelineHost(&basic) && registerCalls == 1 && idleRegistrations == 1,
        "repeated setup does not duplicate registered hooks");
    l2dae::TimelineCommand command;
    command.oldBinding = 101; command.newBinding = 102;
    command.slot = 8; command.duration = 2.75;
    command.label = L"中文 \\\"; dangerous(); //\n\u2028\U0001f600";
    l2dae::enqueueTimelineCommand(&basic, command);
    require(scripts.empty(), "enqueue does not run script during effect callback");
    command.oldBinding = 102; command.newBinding = 103;
    l2dae::enqueueTimelineCommand(&basic, command);
    tick();
    require(scripts.size() == 1 && errors.empty(), "one script per idle tick, strict OK accepted");
    const auto payloadStart = scripts[0].rfind(";L2DAE_addClips(");
    require(payloadStart != std::string::npos, "own installed helper is followed by one explicit clip request");
    const auto payload = scripts[0].substr(payloadStart);
    require(payload.find("\"binding\":102") != std::string::npos && payload.find("\"previousBinding\":101") != std::string::npos &&
        payload.find("\"slot\":8") != std::string::npos && payload.find("\"duration\":2.75") != std::string::npos &&
        payload.find("\"append\":true") != std::string::npos && payload.find("\"kind\":\"motion\"") != std::string::npos,
        "typed clip payload retains kind/identity/slot/duration/mode");
    require(payload.find("\"transitionFrames\":30") != std::string::npos, "default 30-frame transition travels through native bridge");
    require(payload.find("\"transitionCurve\":0") != std::string::npos, "legacy native callers retain uniform transitions");
    require(payload.find("\\u4e2d\\u6587") != std::string::npos && payload.find("\\u000a") != std::string::npos &&
        payload.find("\\u2028") != std::string::npos && payload.find("\\ud83d\\ude00") != std::string::npos &&
        payload.find("\\\"") != std::string::npos, "Unicode, quotes and script line breaks safely encoded inside JSON");
    tick();
    require(scripts.size() == 2, "second pending request executes exactly once");
    tick(); require(scripts.size() == 2, "empty idle cannot repeat a command");
    command.expression = true; command.append = false;
    l2dae::enqueueTimelineCommand(&basic, command); tick();
    const auto expressionPayload = scripts.back().substr(scripts.back().rfind(";L2DAE_addClips("));
    require(expressionPayload.find("\"kind\":\"expression\"") != std::string::npos &&
        expressionPayload.find("\"append\":false") != std::string::npos, "expression clips use explicit isolated kind and placement mode");
    command.expression = false;
    mode = ResultMode::Error;
    l2dae::enqueueTimelineCommand(&basic, command); tick();
    require(errors.size() == 1 && errors.back().find("fixture script failure") != std::string::npos, "script error reported with its text");
    mode = ResultMode::Unexpected;
    l2dae::enqueueTimelineCommand(&basic, command); tick();
    require(errors.size() == 2, "unexpected successful result is not mistaken for clip creation");
    mode = ResultMode::Success;
    l2dae::enqueueTimelineCommand(&basic, command);
    const size_t beforeShutdown = scripts.size();
    l2dae::shutdownTimelineHost(); tick();
    require(scripts.size() == beforeShutdown, "global teardown clears pending work");
    require(l2dae::initializeTimelineHost(&basic) && registerCalls == 1 && idleRegistrations == 1,
        "setup after teardown safely reuses the registered idle hook");
    scriptingAvailable = false;
    bool rejected = false;
    try { l2dae::enqueueTimelineCommand(&basic, command); } catch (const std::exception&) { rejected = true; }
    require(rejected, "unavailable scripting fails before queueing");
    scriptingAvailable = true;
    command.duration = std::numeric_limits<double>::quiet_NaN(); rejected = false;
    try { l2dae::enqueueTimelineCommand(&basic, command); } catch (const std::exception&) { rejected = true; }
    require(rejected, "nonfinite clip duration rejected before script generation");
    std::int32_t token = 0;
    for (int i = 0; i < 32; ++i) {
        const auto next = l2dae::newTimelineBinding(token);
        require(next > 0 && next != token, "fresh positive int32 binding per import"); token = next;
    }
    command.duration = 2.75; command.slot = 1024; command.oldBinding = 201; command.newBinding = 202;
    command.transitionCurve = 1; command.transitionFrames = 15;
    auto expressionCommand = command; expressionCommand.expression = true; expressionCommand.oldBinding = 301; expressionCommand.newBinding = 302;
    expressionCommand.append = false; expressionCommand.transitionFrames = 24; expressionCommand.transitionCurve = 0;
    const auto beforePair = scripts.size();
    l2dae::enqueueTimelineCommands(&basic, {command, expressionCommand}); tick();
    require(scripts.size() == beforePair + 1 && scripts.back().find("\"slot\":1024") != std::string::npos &&
        scripts.back().find("\"transitionFrames\":24") != std::string::npos, "paired unlimited-index import executes as one script batch");
    const auto pairedPayload = scripts.back().substr(scripts.back().rfind(";L2DAE_addClips("));
    require(pairedPayload.find("\"transitionFrames\":15") != std::string::npos &&
        pairedPayload.find("\"transitionFrames\":24") != std::string::npos,
        "explicit saved motion and expression transition durations are not replaced by the new default");
    require(pairedPayload.find("\"transitionCurve\":1") != std::string::npos && pairedPayload.find("\"transitionCurve\":0") != std::string::npos,
        "paired native bridge transmits independent chosen curve values");
    for (int curve : {3, 4, 5}) {
        command.transitionCurve = curve;
        const auto beforeAmount = scripts.size();
        l2dae::enqueueTimelineCommands(&basic, {command, expressionCommand}); tick();
        require(scripts.size() == beforeAmount + 1, "each rebound amount queues a single atomic batch");
        const auto amountPayload = scripts.back().substr(scripts.back().rfind(";L2DAE_addClips("));
        require(amountPayload.find("\"transitionCurve\":" + std::to_string(curve)) != std::string::npos &&
            amountPayload.find("\"transitionCurve\":0") != std::string::npos,
            "no/gentle/pronounced rebound crosses native script bridge without affecting expression fade");
    }
    rejected = false; try { l2dae::enqueueTimelineCommands(&basic, {command, command}); } catch (...) { rejected = true; }
    require(rejected, "duplicate motion kind is rejected before queue acceptance");
    command.slot = 16777217; rejected = false;
    try { l2dae::enqueueTimelineCommand(&basic, command); } catch (...) { rejected = true; }
    require(rejected, "index beyond exact float precision is rejected");
    command.slot = 1024; command.transitionFrames = std::numeric_limits<double>::infinity(); rejected = false;
    try { l2dae::enqueueTimelineCommand(&basic, command); } catch (...) { rejected = true; }
    require(rejected, "nonfinite transition frames are rejected");
    command.transitionFrames = 15; command.transitionCurve = 6; rejected = false;
    try { l2dae::enqueueTimelineCommand(&basic, command); } catch (...) { rejected = true; }
    require(rejected, "unknown transition curve is rejected before enqueue");
    l2dae::shutdownTimelineHost();
    require(suiteReferences == 0 && handles.empty(), "bridge tests leave no acquired host resources");
    std::cout << "PASS: deferred native timeline bridge, JSON escaping, idle reentrancy, strict script result/errors, lifecycle and handle cleanup (mock AEGP).\n";
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: AdapterSmoke AeGOFlash.aex [model.model3.json]");
        HMODULE module = LoadLibraryW(argv[1]);
        require(module != nullptr, "AEX loads with its Windows system dependencies");
        auto effect = reinterpret_cast<Effect>(GetProcAddress(module, "EffectMain"));
        auto reg = reinterpret_cast<PluginDataEntryFunction2Ptr>(GetProcAddress(module, "PluginDataEntryFunction2"));
        auto legacyReg = reinterpret_cast<PluginDataEntryFunctionPtr>(GetProcAddress(module, "PluginDataEntryFunction"));
        require(effect && reg && legacyReg, "effect plus both discovery generations are exported");
        require(reg(nullptr, registration, nullptr, "After Effects", "26.5") == 0, "modern registration callback");
        require(reg(nullptr, nullptr, nullptr, nullptr, nullptr) == PF_Err_INVALID_CALLBACK, "null registration rejected");
        require(legacyReg(nullptr, nullptr, nullptr, nullptr, nullptr) == PF_Err_INVALID_CALLBACK, "null legacy registration rejected");
        PF_UtilCallbacks utils{};
        utils.host_new_handle = allocate;
        utils.host_lock_handle = lock;
        utils.host_unlock_handle = unlock;
        utils.host_dispose_handle = dispose;
        utils.host_get_handle_size = sizeOf;
        PF_InData in{};
        in.utils = &utils;
        in.inter.add_param = addParam;
        in.inter.abort = abortNone;
        encodingMock::run(effect, legacyReg, reg, in);
        require(reg(nullptr, registration, nullptr, "After Effects", "26.5") == 0, "restore modern registration after legacy encoding probes");
        PF_OutData out{};
        require(effect(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) == 0, "global setup");
        require(out.out_flags == 0x02100406 && out.out_flags2 == 0x08000088 && out.my_version == 953857, "runtime/PiPL flags agree");
        require((out.out_flags2 & PF_OutFlag2_SUPPORTS_THREADED_RENDERING) &&
            !(out.out_flags2 & (PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                PF_OutFlag2_MUTABLE_RENDER_SEQUENCE_DATA_SLOWER)) &&
            !(out.out_flags & PF_OutFlag_SEQUENCE_DATA_NEEDS_FLATTENING),
            "legacy RENDER opts into MFR without claiming SmartFX, float or mutable sequence data");
        require(PF_Version_STAGE(out.my_version) == PF_Stage_RELEASE && out.my_version > 951809,
            "AeGO Flash 1.0.1 is a release with an AE compatibility version monotonically increasing for saved-project compatibility");
        capturedImportTransitionRegistration = false;
        require(effect(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) == 0, "parameter setup");
        require(out.num_params == 46 && parameters.size() == 46 && kParameterCount == 46,
            "37 persistent controls plus input and four start/end topic pairs");
        require(parameters[kSelection].param_type == PF_Param_ARBITRARY_DATA, "persistent arbitrary parameter");
        require(capturedImportTransitionRegistration && importTransitionRegistration.uu.id == 133 &&
            importTransitionRegistration.param_type == PF_Param_FLOAT_SLIDER,
            "capture the actual persistent transition registration before new-effect initialization");
        require(importTransitionRegistration.flags ==
            (PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS),
            "transition preference distinguishes new-effect default from missing old-project value");
        require(importTransitionRegistration.u.fs_d.value == 15 && importTransitionRegistration.u.fs_d.dephault == 30,
            "old projects lacking the transition parameter retain 15 frames while new and reset effects use 30");
        require(parameters[kImportTransitionFrames].u.fs_d.value == 30 &&
            parameters[kImportTransitionFrames].u.fs_d.dephault == 30,
            "fake host instantiates the new effect from the registered 30-frame default");
        require(parameters[kImportTransitionFrames].u.fs_d.valid_min == 0 &&
            parameters[kImportTransitionFrames].u.fs_d.valid_max == 100000 &&
            parameters[kImportTransitionFrames].u.fs_d.precision == 0 &&
            !(parameters[kImportTransitionFrames].ui_flags & (PF_PUI_INVISIBLE | PF_PUI_NO_ECW_UI)),
            "default transition remains a visible whole-frame preference with its original saved range");
        // Independent expected UI order: do not merely agree with the table used
        // by production code. Every old disk ID must still occur exactly once.
        constexpr std::array<A_long, 46> expectedDiskOrder{
            0, 201, 101, 102, 116, 133, 202, 203, 106, 107, 108, 204,
            205, 123, 124, 125, 206, 207, 126, 128, 129, 127, 131, 208,
            103, 104, 105, 109, 110, 111, 112, 113, 114, 115,
            117, 118, 119, 120, 121, 122, 130, 132, 134, 135, 136, 137};
        for (size_t i = 1; i < parameters.size(); ++i) {
            require(parameters[i].uu.id == expectedDiskOrder[i], "registered order matches grouped panel layout");
            require(kParameterDiskIds[i] == expectedDiskOrder[i], "runtime enum maps to the independent saved-ID schema");
        }
        for (A_long id = 101; id <= 137; ++id)
            require(std::count_if(parameters.begin(), parameters.end(), [id](const auto& p) { return p.uu.id == id; }) == 1,
                "all 37 historical disk IDs survive reordering exactly once");
        for (const auto& pair : std::array<std::array<size_t, 2>, 4>{{
                {{kModelGroupStart, kModelGroupEnd}}, {{kLayoutGroupStart, kLayoutGroupEnd}},
                {{kAudioGroupStart, kAudioGroupEnd}}, {{kAmbientGroupStart, kAmbientGroupEnd}}}}) {
            require(parameters[pair[0]].param_type == PF_Param_GROUP_START &&
                parameters[pair[1]].param_type == PF_Param_GROUP_END, "each panel group has balanced native topic boundaries");
            const bool collapsed = pair[0] == kAudioGroupStart || pair[0] == kAmbientGroupStart;
            require(bool(parameters[pair[0]].flags & PF_ParamFlag_START_COLLAPSED) == collapsed,
                "only optional audio and ambient groups start collapsed");
        }
        for (size_t i : {kMotionA, kMotionB}) {
            require(parameters[i].param_type == PF_Param_POPUP && parameters[i].u.pd.num_choices == 8, "eight motion slots");
            require(!(parameters[i].flags & PF_ParamFlag_CANNOT_TIME_VARY) &&
                (parameters[i].flags & PF_ParamFlag_CANNOT_INTERP), "slot choice supports held keyframes");
        }
        require(parameters[kMotionA].u.pd.value == 1 && parameters[kMotionB].u.pd.value == 2, "default A/B slots");
        require(parameters[kManualTime].param_type == PF_Param_CHECKBOX && !parameters[kManualTime].u.bd.value &&
            (parameters[kManualTime].flags & PF_ParamFlag_CANNOT_TIME_VARY), "automatic timing remains old-project default");
        for (size_t i : {kMotionTimeA, kMotionTimeB, kBlend}) {
            require(parameters[i].param_type == PF_Param_FLOAT_SLIDER && parameters[i].u.fs_d.value == 0, "time/blend defaults");
            require(!(parameters[i].flags & (PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP)), "time/blend support interpolated keyframes");
        }
        require(parameters[kBlend].u.fs_d.valid_min == 0 && parameters[kBlend].u.fs_d.valid_max == 110,
            "native blend stream accepts the small real overshoot above 100 percent");
        for (size_t i : {kLoop, kSpeed, kStartTime, kMotionA, kMotionB, kManualTime, kMotionTimeA, kMotionTimeB, kBlend})
            require((parameters[i].ui_flags & PF_PUI_NO_ECW_UI) && !(parameters[i].ui_flags & PF_PUI_INVISIBLE),
                "legacy animation streams stay in Timeline, outside simple Effect Controls");
        for (size_t i : {kScale, kOffsetX, kOffsetY})
            require(!(parameters[i].ui_flags & (PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE)), "placement controls remain visible");
        require(parameters[kTimelineBinding].param_type == PF_Param_FLOAT_SLIDER && parameters[kTimelineBinding].u.fs_d.value == 0 &&
            (parameters[kTimelineBinding].ui_flags & PF_PUI_INVISIBLE) && (parameters[kTimelineBinding].flags & PF_ParamFlag_CANNOT_TIME_VARY),
            "new hidden nonanimated binding has safe old-project default");
        require(parameters[kImportExpression].param_type == PF_Param_BUTTON && (parameters[kImportExpression].flags & PF_ParamFlag_SUPERVISE) &&
            !(parameters[kImportExpression].ui_flags & (PF_PUI_INVISIBLE | PF_PUI_NO_ECW_UI)), "expression import is visible and supervised");
        require(parameters[kExpressionSelection].param_type == PF_Param_ARBITRARY_DATA && parameters[kExpressionSelection].u.arb_d.id == 117 &&
            (parameters[kExpressionSelection].flags & PF_ParamFlag_CANNOT_TIME_VARY), "expression paths have a separate stable arbitrary parameter");
        for (size_t i : {kExpressionA, kExpressionB}) {
            require(parameters[i].param_type == PF_Param_POPUP && parameters[i].u.pd.num_choices == 8 &&
                (parameters[i].ui_flags & PF_PUI_NO_ECW_UI) && (parameters[i].flags & PF_ParamFlag_CANNOT_INTERP),
                "expression slots are held timeline controls");
        }
        require(parameters[kExpressionA].u.pd.value == 1 && parameters[kExpressionB].u.pd.value == 2, "default expression A/B slots");
        for (size_t i : {kExpressionWeightA, kExpressionWeightB})
            require(parameters[i].param_type == PF_Param_FLOAT_SLIDER && parameters[i].u.fs_d.value == 0 &&
                parameters[i].u.fs_d.valid_max == 100 && (parameters[i].ui_flags & PF_PUI_NO_ECW_UI) &&
                !(parameters[i].flags & (PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP)),
                "expression weights default inactive and allow interpolated keyframes");
        require(parameters[kExpressionBinding].param_type == PF_Param_FLOAT_SLIDER && parameters[kExpressionBinding].u.fs_d.value == 0 &&
            (parameters[kExpressionBinding].ui_flags & PF_PUI_INVISIBLE) && (parameters[kExpressionBinding].flags & PF_ParamFlag_CANNOT_TIME_VARY),
            "expression clips have an independent hidden binding");
        require(parameters[kAudioLayer].param_type == PF_Param_LAYER && parameters[kAudioLayer].u.ld.dephault == PF_LayerDefault_NONE &&
            (parameters[kAudioLayer].flags & PF_ParamFlag_CANNOT_TIME_VARY) && !(parameters[kAudioLayer].flags & PF_ParamFlag_SUPERVISE),
            "native audio-layer selector defaults to none without initialization side effects");
        require(parameters[kLipSync].param_type == PF_Param_CHECKBOX && !parameters[kLipSync].u.bd.value &&
            !parameters[kLipSync].u.bd.dephault && (parameters[kLipSync].flags & PF_ParamFlag_CANNOT_TIME_VARY),
            "old projects bypass all audio callbacks until the user explicitly enables lip sync");
        require(parameters[kLipSensitivity].param_type == PF_Param_FLOAT_SLIDER && parameters[kLipSensitivity].u.fs_d.value == 100 &&
            parameters[kLipSensitivity].u.fs_d.valid_min == 0 && parameters[kLipSensitivity].u.fs_d.valid_max == 500 &&
            !(parameters[kLipSensitivity].flags & PF_ParamFlag_CANNOT_TIME_VARY), "audio sensitivity defaults to 100 percent and supports keys");
        for (size_t i : {kAudioLayer, kLipSync, kLipSensitivity})
            require(!(parameters[i].ui_flags & (PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE)), "audio controls are visible");
        for (size_t i : {kBreathing, kAutoBlink}) {
            require(parameters[i].param_type == PF_Param_CHECKBOX && !parameters[i].u.bd.value &&
                !parameters[i].u.bd.dephault, "ambient switches default off for new and old projects");
            require((parameters[i].flags & PF_ParamFlag_CANNOT_INTERP) &&
                !(parameters[i].flags & PF_ParamFlag_CANNOT_TIME_VARY), "ambient switches support held keyframes");
            require(!(parameters[i].ui_flags & (PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE)), "ambient switches are visible");
        }
        const auto decodedLabel = [](const char* text) {
            wchar_t decoded[128]{};
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, decoded, 128);
            if (!size) throw std::runtime_error("Invalid multibyte effect UI label.");
            return std::wstring(decoded, static_cast<size_t>(size - 1));
        };
        require(decodedLabel(parameters[kBreathing].PF_DEF_NAME) == L"呼吸动画" &&
            decodedLabel(parameters[kAutoBlink].PF_DEF_NAME) == L"自动眨眼", "localized independent breathing and blink controls decode as UTF-8 for AE 2026");

        for (size_t index : {kBlinkStrength, kBlinkDuration}) {
            const auto& control = parameters[index];
            require(control.param_type == PF_Param_FLOAT_SLIDER && control.uu.id == kParameterDiskIds[index] &&
                (control.ui_flags & PF_PUI_INVISIBLE),
                "retired blink strength/duration preserve disk IDs but disappear from both Effect Controls and Timeline");
            require(!(control.flags & (PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP)),
                "retired blink streams still accept host-evaluated legacy values and keyframes");
        }
        require(parameters[kBlinkStrength].u.fs_d.value == 100 && parameters[kBlinkStrength].u.fs_d.dephault == 100 &&
            parameters[kBlinkDuration].u.fs_d.value == .3f && parameters[kBlinkDuration].u.fs_d.dephault == .3f,
            "fresh effects keep full blink strength and default 0.3-second duration");
        require(parameters[kBlinkInterval].param_type == PF_Param_FLOAT_SLIDER && parameters[kBlinkInterval].uu.id == 131 &&
            parameters[kBlinkInterval].u.fs_d.value == 4 && !(parameters[kBlinkInterval].ui_flags & (PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE)) &&
            !(parameters[kBlinkInterval].flags & PF_ParamFlag_CANNOT_TIME_VARY),
            "blink interval remains visible and keyframeable beside the Auto blink switch");
        for (size_t index : {kMotionAIndex, kMotionBIndex, kExpressionAIndex, kExpressionBIndex}) {
            const auto& control = parameters[index];
            require(control.param_type == PF_Param_FLOAT_SLIDER && control.u.fs_d.value == 0 &&
                control.u.fs_d.valid_min == 0 && control.u.fs_d.valid_max == 16777216,
                "dynamic indices retain exact float range and legacy zero fallback");
            require((control.ui_flags & PF_PUI_NO_ECW_UI) && !(control.ui_flags & PF_PUI_INVISIBLE) &&
                !(control.flags & (PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP)),
                "REGRESSION: animation index streams must be available to host expressions, hidden only from Effect Controls");
        }
        auto arb = [&](PF_ArbParamsExtra& extra, A_short diskId = 102) {
            extra.id = diskId;
            return effect(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, &extra);
        };
        PF_ArbParamsExtra e{};
        auto disposeArb = [&](PF_ArbitraryH h, A_short diskId = 102) {
            e = {}; e.which_function = PF_Arbitrary_DISPOSE_FUNC;
            e.u.dispose_func_params.arbH = h;
            require(arb(e, diskId) == 0, "arb disposal callback");
        };
        auto scanText = [&](const std::string& value, A_short diskId = 102) {
            PF_ArbitraryH result{};
            PF_ArbParamsExtra extra{}; extra.which_function = PF_Arbitrary_SCAN_FUNC;
            extra.u.scan_func_params.bufPC = value.data();
            extra.u.scan_func_params.bytes_to_scanLu = static_cast<A_u_long>(value.size());
            extra.u.scan_func_params.arbPH = &result;
            require(arb(extra, diskId) == 0 && result, "scan valid saved selection");
            return result;
        };
        auto toText = [&](PF_ArbitraryH value, A_short diskId = 102) {
            A_u_long count{};
            PF_ArbParamsExtra extra{}; extra.which_function = PF_Arbitrary_PRINT_SIZE_FUNC;
            extra.u.print_size_func_params.arbH = value;
            extra.u.print_size_func_params.print_sizePLu = &count;
            require(arb(extra, diskId) == 0 && count, "query printed size");
            std::vector<char> result(count);
            extra = {}; extra.which_function = PF_Arbitrary_PRINT_FUNC;
            extra.u.print_func_params.arbH = value;
            extra.u.print_func_params.print_sizeLu = count;
            extra.u.print_func_params.print_bufferPC = result.data();
            require(arb(extra, diskId) == 0, "print saved selection");
            return std::string(result.data());
        };
        const std::string legacyModel = "C:\\模型\\a.model3.json";
        const std::string legacyMotion = "C:\\动作\\a.motion3.json";
        const std::string legacyText = "Live2DNative1|" + hexText(legacyModel) + "|" + hexText(legacyMotion);
        const std::string migratedText = "Live2DNative3|8|" + hexText(legacyModel) + "|" + hexText(legacyMotion) + "|||||||";
        auto legacyFromText = scanText(legacyText);
        require(toText(legacyFromText) == migratedText, "v1 clipboard migrates to slot 1; other slots empty");
        disposeArb(legacyFromText);
        // Build the exact v1 on-disk payload and an old in-memory handle, without
        // relying on the new implementation's structs or serialization helpers.
        std::vector<unsigned char> legacyWire(16396, 0);
        std::memcpy(legacyWire.data(), "L2DAE001", 8);
        legacyWire[8] = 1;
        std::memcpy(legacyWire.data() + 12, legacyModel.c_str(), legacyModel.size());
        std::memcpy(legacyWire.data() + 12 + 8192, legacyMotion.c_str(), legacyMotion.size());
        PF_ArbitraryH legacyFromWire{};
        e = {}; e.which_function = PF_Arbitrary_UNFLATTEN_FUNC;
        e.u.unflatten_func_params.buf_sizeLu = static_cast<A_u_long>(legacyWire.size());
        e.u.unflatten_func_params.flat_dataPV = legacyWire.data();
        e.u.unflatten_func_params.arbPH = &legacyFromWire;
        require(arb(e) == 0 && toText(legacyFromWire) == migratedText, "v1 binary project data migrates");
        disposeArb(legacyFromWire);
        const auto legacyHandle = allocate(16388);
        std::memcpy(*legacyHandle, legacyWire.data() + 8, 16388);
        PF_ArbitraryH migratedHandle{};
        e = {}; e.which_function = PF_Arbitrary_COPY_FUNC;
        e.u.copy_func_params.src_arbH = legacyHandle;
        e.u.copy_func_params.dst_arbPH = &migratedHandle;
        require(arb(e) == 0 && sizeOf(migratedHandle) == 16, "live v1 handle migrates to independent fixed-size owning header");
        require(toText(migratedHandle) == migratedText, "live v1 motion preserved in slot 1");
        disposeArb(legacyHandle); disposeArb(migratedHandle);
        PF_ArbitraryH blank{};
        e.which_function = PF_Arbitrary_NEW_FUNC;
        e.u.new_func_params.arbPH = &blank;
        require(arb(e) == 0 && blank, "new state");
        std::string text = "Live2DNative2|" + hexText(legacyModel);
        for (int i = 0; i < 8; ++i)
            text += "|" + hexText("D:\\动作库\\动作" + std::to_string(i + 1) + ".motion3.json");
        PF_ArbitraryH original{};
        e = {}; e.which_function = PF_Arbitrary_SCAN_FUNC;
        e.u.scan_func_params.bufPC = text.data();
        e.u.scan_func_params.bytes_to_scanLu = static_cast<A_u_long>(text.size());
        e.u.scan_func_params.arbPH = &original;
        require(arb(e) == 0 && original, "Unicode model and all eight motion slots scan");
        text = toText(original);
        require(text.find("Live2DNative3|8|") == 0, "legacy v2 text upgrades to variable bank");
        PF_ArbitraryH copy{};
        e = {}; e.which_function = PF_Arbitrary_COPY_FUNC;
        e.u.copy_func_params.src_arbH = original;
        e.u.copy_func_params.dst_arbPH = &copy;
        require(arb(e) == 0 && copy != original && *copy != *original, "undo/copy owns independent storage");
        A_u_long flatSize{};
        e = {}; e.which_function = PF_Arbitrary_FLAT_SIZE_FUNC;
        e.u.flat_size_func_params.arbH = original;
        e.u.flat_size_func_params.flat_data_sizePLu = &flatSize;
        require(arb(e) == 0 && flatSize > 16 && flatSize < 73740, "motion v3 wire has variable-length paths");
        std::vector<unsigned char> bytes(flatSize + 8, 0xcd);
        e = {}; e.which_function = PF_Arbitrary_FLATTEN_FUNC;
        e.u.flatten_func_params.arbH = original;
        e.u.flatten_func_params.buf_sizeLu = flatSize;
        e.u.flatten_func_params.flat_dataPV = bytes.data();
        require(arb(e) == 0 && std::memcmp(bytes.data(), "L2DAE003", 8) == 0 && bytes[8] == 3, "v2 flatten wire magic/version");
        require(bytes[flatSize] == 0xcd && bytes.back() == 0xcd, "flatten preserves trailing guard");
        e.u.flatten_func_params.buf_sizeLu = flatSize - 1;
        require(arb(e) == PF_Err_BAD_CALLBACK_PARAM, "short flatten buffer rejected");
        disposeArb(original);
        original = nullptr;
        e = {}; e.which_function = PF_Arbitrary_UNFLATTEN_FUNC;
        e.u.unflatten_func_params.buf_sizeLu = flatSize;
        e.u.unflatten_func_params.flat_dataPV = bytes.data();
        e.u.unflatten_func_params.arbPH = &original;
        require(arb(e) == 0 && original, "save/reload unflatten");
        PF_ArbCompareResult order{};
        e = {}; e.which_function = PF_Arbitrary_COMPARE_FUNC;
        e.u.compare_func_params.a_arbH = original;
        e.u.compare_func_params.b_arbH = copy;
        e.u.compare_func_params.compareP = &order;
        require(arb(e) == 0 && order == PF_ArbCompare_EQUAL, "save/reload compares equal to independent copy");
        PF_ArbitraryH interpolated{};
        e = {}; e.which_function = PF_Arbitrary_INTERP_FUNC;
        e.u.interp_func_params.left_arbH = original;
        e.u.interp_func_params.right_arbH = blank;
        e.u.interp_func_params.tF = 0.99;
        e.u.interp_func_params.interpPH = &interpolated;
        require(arb(e) == 0 && toText(interpolated) == toText(original), "discrete hold interpolation");
        A_u_long printSize{};
        e = {}; e.which_function = PF_Arbitrary_PRINT_SIZE_FUNC;
        e.u.print_size_func_params.arbH = original;
        e.u.print_size_func_params.print_sizePLu = &printSize;
        require(arb(e) == 0 && printSize == text.size() + 1, "clipboard print size");
        std::vector<char> printed(printSize);
        e = {}; e.which_function = PF_Arbitrary_PRINT_FUNC;
        e.u.print_func_params.arbH = original;
        e.u.print_func_params.print_sizeLu = printSize;
        e.u.print_func_params.print_bufferPC = printed.data();
        require(arb(e) == 0 && text == printed.data(), "Unicode clipboard round trip");
        const size_t before = allocations.size();
        PF_ArbitraryH corrupt{};
        bytes[0] = '?';
        e = {}; e.which_function = PF_Arbitrary_UNFLATTEN_FUNC;
        e.u.unflatten_func_params.buf_sizeLu = flatSize;
        e.u.unflatten_func_params.flat_dataPV = bytes.data();
        e.u.unflatten_func_params.arbPH = &corrupt;
        require(arb(e) != 0 && !corrupt && allocations.size() == before, "corrupt wire safely rejected without allocation");
        // All fields are validated, including currently unused slots.
        bytes[0] = 'L';
        bytes[flatSize - 1] = 0xff;
        require(arb(e) != 0 && !corrupt && allocations.size() == before, "invalid UTF-8 in slot 8 rejected");
        bytes[12] = bytes[13] = bytes[14] = bytes[15] = 0xff;
        require(arb(e) != 0 && !corrupt && allocations.size() == before, "impossible motion count rejected before allocation");
        for (const std::string& invalidText : std::array<std::string, 4>{"Live2DNative2||", text + "|", "Live2DNative2|ff||||||||", "Live2DNative1||00"}) {
            PF_ArbParamsExtra invalid{}; invalid.which_function = PF_Arbitrary_SCAN_FUNC;
            invalid.u.scan_func_params.bufPC = invalidText.data();
            invalid.u.scan_func_params.bytes_to_scanLu = static_cast<A_u_long>(invalidText.size());
            invalid.u.scan_func_params.arbPH = &corrupt;
            require(arb(invalid) == PF_Err_CANNOT_PARSE_KEYFRAME_TEXT && !corrupt && allocations.size() == before,
                "malformed clipboard fields rejected without allocation");
        }
        // Independent expression-bank schema: all eleven callbacks and malformed
        // fields are tested without changing the existing motion-v2 assertions.
        PF_ArbitraryH expressionBlank{};
        e = {}; e.which_function = PF_Arbitrary_NEW_FUNC;
        e.u.new_func_params.arbPH = &expressionBlank;
        require(arb(e, 117) == 0 && sizeOf(expressionBlank) == 16, "new independent expression bank");
        require(toText(expressionBlank, 117) == "Live2DExpression2|0", "empty expression clipboard schema");
        std::string expressionText = "Live2DExpression1|";
        for (int i = 0; i < 8; ++i) {
            if (i) expressionText += '|';
            expressionText += hexText("D:\\表情库\\表情" + std::to_string(i + 1) + ".exp3.json");
        }
        auto expressionOriginal = scanText(expressionText, 117);
        expressionText = toText(expressionOriginal, 117);
        require(expressionText.find("Live2DExpression2|8|") == 0, "legacy expression text upgrades");
        PF_ArbitraryH expressionCopy{};
        e = {}; e.which_function = PF_Arbitrary_COPY_FUNC;
        e.u.copy_func_params.src_arbH = expressionOriginal;
        e.u.copy_func_params.dst_arbPH = &expressionCopy;
        require(arb(e, 117) == 0 && expressionCopy != expressionOriginal && *expressionCopy != *expressionOriginal,
            "expression copies own independent storage");
        A_u_long expressionSize{};
        e = {}; e.which_function = PF_Arbitrary_FLAT_SIZE_FUNC;
        e.u.flat_size_func_params.arbH = expressionOriginal;
        e.u.flat_size_func_params.flat_data_sizePLu = &expressionSize;
        require(arb(e, 117) == 0 && expressionSize > 16 && expressionSize < 65548, "expression wire size is separate from unchanged motion-v2 wire");
        std::vector<unsigned char> expressionBytes(expressionSize + 8, 0xcd);
        e = {}; e.which_function = PF_Arbitrary_FLATTEN_FUNC;
        e.u.flatten_func_params.arbH = expressionOriginal;
        e.u.flatten_func_params.buf_sizeLu = expressionSize;
        e.u.flatten_func_params.flat_dataPV = expressionBytes.data();
        require(arb(e, 117) == 0 && !std::memcmp(expressionBytes.data(), "L2DEX002", 8) && expressionBytes[8] == 2 &&
            expressionBytes[expressionSize] == 0xcd && expressionBytes.back() == 0xcd, "expression magic/version and flatten guard");
        e.u.flatten_func_params.buf_sizeLu = expressionSize - 1;
        require(arb(e, 117) == PF_Err_BAD_CALLBACK_PARAM, "expression short flatten buffer rejected");
        disposeArb(expressionOriginal, 117); expressionOriginal = nullptr;
        e = {}; e.which_function = PF_Arbitrary_UNFLATTEN_FUNC;
        e.u.unflatten_func_params.buf_sizeLu = expressionSize;
        e.u.unflatten_func_params.flat_dataPV = expressionBytes.data();
        e.u.unflatten_func_params.arbPH = &expressionOriginal;
        require(arb(e, 117) == 0 && expressionOriginal, "expression reload from saved bytes");
        e = {}; e.which_function = PF_Arbitrary_COMPARE_FUNC;
        e.u.compare_func_params.a_arbH = expressionOriginal;
        e.u.compare_func_params.b_arbH = expressionCopy;
        e.u.compare_func_params.compareP = &order;
        require(arb(e, 117) == 0 && order == PF_ArbCompare_EQUAL, "expression reload compares equal to independent copy");
        PF_ArbitraryH expressionInterpolated{};
        e = {}; e.which_function = PF_Arbitrary_INTERP_FUNC;
        e.u.interp_func_params.left_arbH = expressionOriginal;
        e.u.interp_func_params.right_arbH = expressionBlank;
        e.u.interp_func_params.tF = 0.5;
        e.u.interp_func_params.interpPH = &expressionInterpolated;
        require(arb(e, 117) == 0 && toText(expressionInterpolated, 117) == expressionText, "expression hold interpolation and Unicode print round trip");
        require(toText(original) == text, "expression callbacks leave motion-v2 bank unchanged");
        PF_ArbitraryH badExpression{};
        const auto beforeBadExpression = allocations.size();
        e = {}; e.which_function = PF_Arbitrary_UNFLATTEN_FUNC;
        e.u.unflatten_func_params.buf_sizeLu = expressionSize;
        e.u.unflatten_func_params.flat_dataPV = expressionBytes.data();
        e.u.unflatten_func_params.arbPH = &badExpression;
        expressionBytes[0] = '?';
        require(arb(e, 117) != 0 && !badExpression && allocations.size() == beforeBadExpression, "expression invalid magic rejected without allocation");
        expressionBytes[0] = 'L'; expressionBytes[8] = 99;
        require(arb(e, 117) != 0 && !badExpression && allocations.size() == beforeBadExpression, "expression unknown version rejected");
        expressionBytes[8] = 2; expressionBytes[expressionSize - 1] = 0xff;
        require(arb(e, 117) != 0 && !badExpression && allocations.size() == beforeBadExpression, "expression malformed UTF-8 in last slot rejected");
        expressionBytes[12] = expressionBytes[13] = expressionBytes[14] = expressionBytes[15] = 0xff;
        require(arb(e, 117) != 0 && !badExpression && allocations.size() == beforeBadExpression, "expression impossible count rejected");
        for (const auto& invalid : std::array<std::string, 4>{"Live2DExpression1||", expressionText + "|", "Live2DExpression1|ff|||||||", "Live2DExpression1|00|||||||"}) {
            e = {}; e.which_function = PF_Arbitrary_SCAN_FUNC;
            e.u.scan_func_params.bufPC = invalid.data();
            e.u.scan_func_params.bytes_to_scanLu = static_cast<A_u_long>(invalid.size());
            e.u.scan_func_params.arbPH = &badExpression;
            require(arb(e, 117) == PF_Err_CANNOT_PARSE_KEYFRAME_TEXT && !badExpression && allocations.size() == beforeBadExpression,
                "expression malformed clipboard rejected without allocation");
        }
        disposeArb(expressionBlank, 117); disposeArb(expressionOriginal, 117);
        disposeArb(expressionCopy, 117); disposeArb(expressionInterpolated, 117);

        std::array<PF_ParamDef*, kParameterCount> params{};
        for (size_t i = 0; i < params.size(); ++i) params[i] = &parameters[i];
        PF_UserChangedParamExtra audioChanged{};
        audioChanged.param_index = kAudioLayer;
        parameters[kLipSync].uu.change_flags = 0;
        require(effect(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr, &audioChanged) == 0 &&
            !parameters[kLipSync].u.bd.value && parameters[kLipSync].uu.change_flags == 0,
            "REGRESSION: initialization or None layer notification never enables audio");
        parameters[kLipSync].u.bd.value = TRUE;
        require(effect(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr, &audioChanged) == 0 &&
            parameters[kLipSync].u.bd.value && parameters[kLipSync].uu.change_flags == 0,
            "layer notifications preserve the user's explicit enabled value");
        parameters[kLipSync].u.bd.value = FALSE;
        PF_UserChangedParamExtra importExtra{};
        importExtra.param_index = kOpenDialog;
        const size_t beforeUnavailableImport = allocations.size();
        require(effect(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr, &importExtra) == PF_Err_BAD_CALLBACK_PARAM &&
            allocations.size() == beforeUnavailableImport && parameters[kTimelineBinding].u.fs_d.value == 0,
            "offline host reports unavailable import bridge before opening UI or altering saved state");
        importExtra.param_index = kImportExpression;
        require(effect(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr, &importExtra) == PF_Err_BAD_CALLBACK_PARAM &&
            allocations.size() == beforeUnavailableImport && parameters[kExpressionBinding].u.fs_d.value == 0,
            "offline expression import fails before UI/state/binding mutation");
        for (bool deep : {false, true}) {
            const int rowSize = deep ? 16 : 8;
            std::array<unsigned char, 64> frame;
            frame.fill(0xcd);
            PF_LayerDef world{};
            world.width = 2; world.height = 2;
            world.rowbytes = rowSize + 8;
            world.data = reinterpret_cast<PF_PixelPtr>(frame.data());
            world.world_flags = deep ? PF_WorldFlag_DEEP : 0;
            require(effect(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr) == 0, "empty model render");
            for (int y = 0; y != 2; ++y) {
                for (int x = 0; x < rowSize; ++x) require(frame[y * world.rowbytes + x] == 0, "transparent output");
                for (int x = rowSize; x < world.rowbytes; ++x) require(frame[y * world.rowbytes + x] == 0xcd, "rowbytes guard intact");
            }
        }
        if (argc == 3) {
            const std::string modelHex = hexText(utf8Path(argv[2]));
            PF_ArbitraryH model = scanText("Live2DNative1|" + modelHex + "|");
            parameters[kSelection].u.arb_d.value = model;
            in.current_time = 120; in.time_scale = 60;
            in.downsample_x = {1, 1}; in.downsample_y = {1, 1};
            in.pixel_aspect_ratio = {1, 1};
            constexpr int w = 128, h = 128;
            std::vector<PF_Pixel8> image8(w * h);
            std::vector<PF_Pixel16> image16(w * h);
            PF_LayerDef world{};
            world.width = w; world.height = h;
            world.rowbytes = w * sizeof(PF_Pixel8);
            world.data = image8.data();
            auto err = effect(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr);
            if (err) std::cerr << out.return_msg << '\n';
            require(err == 0, "actual model AEX 8-bpc rendering");
            world.world_flags = PF_WorldFlag_DEEP;
            world.rowbytes = w * sizeof(PF_Pixel16);
            world.data = reinterpret_cast<PF_PixelPtr>(image16.data());
            err = effect(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr);
            if (err) std::cerr << out.return_msg << '\n';
            require(err == 0, "actual model AEX 16-bpc rendering");
            size_t opaque = 0, transparent = 0;
            for (size_t i = 0; i < image8.size(); ++i) {
                const auto& a = image8[i]; const auto& b = image16[i];
                opaque += a.alpha != 0; transparent += a.alpha == 0;
                const auto expand = [](unsigned char v) { return (v * 32768u + 127u) / 255u; };
                require(b.alpha == expand(a.alpha) && b.red == expand(a.red) && b.green == expand(a.green) && b.blue == expand(a.blue), "AE 16-bpc channel order and range");
                require(a.red <= a.alpha && a.green <= a.alpha && a.blue <= a.alpha, "premultiplied alpha");
            }
            require(opaque > 100 && transparent > 100, "visible native model with transparent background");
            std::cout << "Native model rendered through actual AEX: " << opaque << " visible pixels, " << transparent << " transparent pixels; 8/16-bpc channel/range agreement.\n";
            disposeArb(model);

            // The fake host now supplies evaluated parameter values at different
            // composition times, exactly as AE does when it evaluates keyframes.
            std::vector<std::wstring> motions;
            const auto parent = std::filesystem::path(argv[2]).parent_path();
            for (const auto& file : std::filesystem::recursive_directory_iterator(parent)) {
                const auto name = file.path().filename().wstring();
                constexpr wchar_t suffix[] = L".motion3.json";
                if (file.is_regular_file() && name.size() >= 13 &&
                    name.compare(name.size() - 13, 13, suffix) == 0) motions.push_back(file.path().wstring());
            }
            std::sort(motions.begin(), motions.end());
            if (motions.size() >= 2) {
                const auto firstHex = hexText(utf8Path(motions.front()));
                const auto secondHex = hexText(utf8Path(motions.back()));
                auto bank = scanText("Live2DNative2|" + modelHex + "|" + firstHex + "|||||||" + secondHex);
                parameters[kSelection].u.arb_d.value = bank;
                parameters[kMotionA].u.pd.value = 1;
                parameters[kMotionB].u.pd.value = 8;
                parameters[kBlend].u.fs_d.value = 0;
                auto render8 = [&]() {
                    std::vector<PF_Pixel8> image(w * h);
                    PF_LayerDef frame{};
                    frame.width = w; frame.height = h;
                    frame.rowbytes = w * sizeof(PF_Pixel8); frame.data = image.data();
                    const auto result = effect(PF_Cmd_RENDER, &in, &out, params.data(), &frame, nullptr);
                    if (result) std::cerr << out.return_msg << '\n';
                    require(result == 0, "keyframed A/B render succeeds");
                    return image;
                };
                const auto equal = [](const auto& a, const auto& b) {
                    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(PF_Pixel8)) == 0;
                };
                // Legacy automatic mode: (2 - 1) * 2 = 2 seconds on both sources.
                in.current_time = 120;
                parameters[kSpeed].u.fs_d.value = 2;
                parameters[kStartTime].u.fs_d.value = 1;
                parameters[kManualTime].u.bd.value = FALSE;
                const auto automaticA = render8();
                parameters[kBlend].u.fs_d.value = 100;
                const auto automaticB = render8();
                parameters[kManualTime].u.bd.value = TRUE;
                parameters[kMotionTimeA].u.fs_d.value = 2;
                parameters[kMotionTimeB].u.fs_d.value = 2;
                parameters[kBlend].u.fs_d.value = 0;
                in.current_time = 900;
                require(equal(automaticA, render8()), "manual A time ignores layer time/start/speed");
                parameters[kBlend].u.fs_d.value = 100;
                require(equal(automaticB, render8()), "automatic and manual timing agree for B");
                parameters[kMotionTimeB].u.fs_d.value = 0.75;
                const auto endpointB = render8();
                parameters[kMotionA].u.pd.value = 8;
                parameters[kMotionTimeA].u.fs_d.value = 0.75;
                parameters[kBlend].u.fs_d.value = 0;
                require(equal(endpointB, render8()), "100 percent selects independently timed B, equal to direct A slot 8");
                std::string largeMotionText = "Live2DNative3|1024|" + modelHex;
                for (int i = 0; i < 1024; ++i) largeMotionText += "|" + (i == 1023 ? secondHex : firstHex);
                auto largeBank = scanText(largeMotionText);
                parameters[kSelection].u.arb_d.value = largeBank; parameters[kMotionAIndex].u.fs_d.value = 1024;
                require(equal(endpointB, render8()), "actual AEX motion index 1024 renders identically to the legacy source slot");
                parameters[kMotionAIndex].u.fs_d.value = 0; parameters[kSelection].u.arb_d.value = bank;
                disposeArb(largeBank);
                parameters[kMotionA].u.pd.value = 1;
                parameters[kMotionTimeA].u.fs_d.value = 2;
                const auto endpointA = render8();
                parameters[kBlend].u.fs_d.value = 25;
                const auto quarter = render8();
                require(!equal(endpointA, endpointB), "fixture has distinct endpoint poses");
                require(!equal(quarter, endpointA) && !equal(quarter, endpointB), "intermediate transition visibly changes pose");
                parameters[kMotionA].u.pd.value = 8;
                parameters[kMotionB].u.pd.value = 1;
                parameters[kMotionTimeA].u.fs_d.value = 0.75;
                parameters[kMotionTimeB].u.fs_d.value = 2;
                parameters[kBlend].u.fs_d.value = 75;
                require(equal(quarter, render8()), "swapping slots/times complements blend weight");
                parameters[kBlend].u.fs_d.value = 0;
                parameters[kMotionTimeA].u.fs_d.value = 0;
                const auto zero = render8();
                parameters[kMotionTimeA].u.fs_d.value = -12;
                require(equal(zero, render8()), "negative manual time clamps to zero");
                parameters[kMotionTimeA].u.fs_d.value = 0.75;
                require(equal(endpointB, render8()), "reverse seek reproduces keyed source time");
                parameters[kMotionTimeA].u.fs_d.value = 4;
                (void)render8();
                parameters[kMotionTimeA].u.fs_d.value = 0.75;
                require(equal(endpointB, render8()), "jump then reverse seek remains deterministic");
                parameters[kBlend].u.fs_d.value = -100;
                require(equal(endpointB, render8()), "transition below range clamps to A");
                parameters[kBlend].u.fs_d.value = 104.96;
                const auto overshoot = render8();
                require(!equal(endpointA, overshoot), "104.96 percent genuinely extrapolates beyond B rather than clamping to it");
                parameters[kBlend].u.fs_d.value = 110;
                const auto upperLimit = render8();
                parameters[kBlend].u.fs_d.value = 1000;
                require(equal(upperLimit, render8()), "transition above range clamps to the 110 percent safety limit");
                parameters[kBlend].u.fs_d.value = 104.96;
                require(equal(overshoot, render8()), "overshoot remains exact after visiting the upper safety limit");
                parameters[kBlend].u.fs_d.value = std::numeric_limits<double>::quiet_NaN();
                world.world_flags = 0; world.rowbytes = w * sizeof(PF_Pixel8); world.data = image8.data();
                require(effect(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr) == PF_Err_BAD_CALLBACK_PARAM,
                    "nonfinite blend rejected before renderer");
                parameters[kBlend].u.fs_d.value = 0;
                parameters[kMotionA].u.pd.value = 0;
                require(effect(PF_Cmd_RENDER, &in, &out, params.data(), &world, nullptr) == PF_Err_BAD_CALLBACK_PARAM,
                    "invalid slot rejected before indexing bank");
                disposeArb(bank);
                std::cout << "PASS: eight-slot selection, independent keyed A/B time, old automatic timing, transition endpoints/intermediate/symmetry and reverse seeking.\n";
            } else {
                std::cout << "SKIP: transition render checks need at least two .motion3.json files under the supplied model folder.\n";
            }

            // Expression weights are independent of motion time and the motion
            // bank. Inactive slots must not attempt to open missing files.
            auto expressionModel = scanText("Live2DNative1|" + modelHex + "|");
            parameters[kSelection].u.arb_d.value = expressionModel;
            parameters[kMotionA].u.pd.value = 1; parameters[kMotionB].u.pd.value = 2;
            parameters[kManualTime].u.bd.value = TRUE;
            parameters[kMotionTimeA].u.fs_d.value = 0; parameters[kMotionTimeB].u.fs_d.value = 0;
            parameters[kBlend].u.fs_d.value = 0;
            parameters[kExpressionWeightA].u.fs_d.value = 0; parameters[kExpressionWeightB].u.fs_d.value = 0;
            constexpr int expressionWidth = 256, expressionHeight = 256;
            std::vector<PF_Pixel8> expressionImage(expressionWidth * expressionHeight);
            PF_LayerDef expressionWorld{};
            expressionWorld.width = expressionWidth; expressionWorld.height = expressionHeight;
            expressionWorld.rowbytes = expressionWidth * sizeof(PF_Pixel8);
            expressionWorld.data = expressionImage.data();
            const auto renderExpression8 = [&]() {
                const auto result = effect(PF_Cmd_RENDER, &in, &out, params.data(), &expressionWorld, nullptr);
                if (result) std::cerr << out.return_msg << '\n';
                require(result == 0, "actual AEX expression render succeeds");
                return expressionImage;
            };
            const auto expressionEqual = [](const auto& a, const auto& b) {
                return a.size() == b.size() && !std::memcmp(a.data(), b.data(), a.size() * sizeof(PF_Pixel8));
            };
            const auto noExpression = renderExpression8();
            // Drive the actual AEX with AE-evaluated checkbox keys and a fixed
            // static pose, so changes here come from the independent ambient
            // clock rather than from motion playback or expressions.
            const auto ambientSavedTime = in.current_time;
            const auto ambientSavedScale = in.time_scale;
            const auto ambientSavedSpeed = parameters[kSpeed].u.fs_d.value;
            const auto ambientSavedStart = parameters[kStartTime].u.fs_d.value;
            in.time_scale = 1000; in.current_time = 0;
            require(expressionEqual(noExpression, renderExpression8()),
                "default-off breathing/blinking preserve legacy pixels across host times");
            parameters[kBreathing].u.bd.value = TRUE;
            const auto breathLow = renderExpression8();
            in.current_time = 2000;
            const auto breathHigh = renderExpression8();
            require(!expressionEqual(breathLow, breathHigh),
                "actual AEX breathing checkbox animates the supplied model while motion time stays zero");
            parameters[kBreathing].u.bd.value = FALSE; parameters[kAutoBlink].u.bd.value = TRUE;
            in.current_time = 2500;
            const auto eyesOpen = renderExpression8();
            in.current_time = 3125;
            const auto eyesClosed = renderExpression8();
            require(!expressionEqual(eyesOpen, eyesClosed),
                "actual AEX auto-blink checkbox closes the supplied model's eyes independently");
            parameters[kBreathing].u.bd.value = TRUE;
            const auto combinedAmbient = renderExpression8();
            require(!expressionEqual(combinedAmbient, eyesClosed),
                "breathing can run together with blinking");
            parameters[kSpeed].u.fs_d.value = 80; parameters[kStartTime].u.fs_d.value = -100;
            require(expressionEqual(combinedAmbient, renderExpression8()),
                "ambient animation does not follow legacy playback speed or start controls");
            parameters[kMotionTimeA].u.fs_d.value = 12; parameters[kMotionTimeB].u.fs_d.value = 19;
            require(expressionEqual(combinedAmbient, renderExpression8()),
                "ambient phase stays fixed when independent A/B clocks change on a static pose");
            in.current_time = 500;
            require(!expressionEqual(combinedAmbient, renderExpression8()),
                "changing host time drives ambient animation even with fixed A/B clocks");
            in.current_time = 3125;
            require(expressionEqual(combinedAmbient, renderExpression8()),
                "ambient reverse seek reproduces the same actual AEX pixels");
            parameters[kBreathing].u.bd.value = FALSE;
            require(expressionEqual(eyesClosed, renderExpression8()),
                "disabling breathing retains the independently enabled blink");
            parameters[kMotionTimeA].u.fs_d.value = 0; parameters[kMotionTimeB].u.fs_d.value = 0;
            parameters[kBreathing].u.bd.value = parameters[kAutoBlink].u.bd.value = TRUE;
            params[kBreathing] = params[kAutoBlink] = nullptr;
            require(expressionEqual(noExpression, renderExpression8()),
                "actual AEX treats missing ambient parameter values as disabled");
            params[kBreathing] = &parameters[kBreathing]; params[kAutoBlink] = &parameters[kAutoBlink];
            parameters[kBreathing].u.bd.value = parameters[kAutoBlink].u.bd.value = FALSE;
            require(expressionEqual(noExpression, renderExpression8()),
                "turning both ambient switches off restores exact legacy pixels");
            in.current_time = ambientSavedTime; in.time_scale = ambientSavedScale;
            parameters[kSpeed].u.fs_d.value = ambientSavedSpeed; parameters[kStartTime].u.fs_d.value = ambientSavedStart;
            std::cout << "PASS: actual AEX independent breathing/blinking keys; host clock independent of A/B time, speed and start; combined operation, reverse seeking, missing values and exact disabled restoration.\n";
            auto missingExpression = scanText("Live2DExpression1|" + hexText("Z:\\missing-live2d-test\\missing.exp3.json") + "|||||||", 117);
            parameters[kExpressionSelection].u.arb_d.value = missingExpression;
            parameters[kExpressionA].u.pd.value = 0; parameters[kExpressionB].u.pd.value = 0;
            require(expressionEqual(noExpression, renderExpression8()), "zero weights skip invalid selectors and missing expression files");
            parameters[kExpressionWeightA].u.fs_d.value = -1;
            require(expressionEqual(noExpression, renderExpression8()), "negative expression weight clamps to inactive");
            parameters[kExpressionA].u.pd.value = 1;
            parameters[kExpressionWeightA].u.fs_d.value = 100;
            require(effect(PF_Cmd_RENDER, &in, &out, params.data(), &expressionWorld, nullptr) != 0,
                "active missing expression reports a render error");
            parameters[kExpressionWeightA].u.fs_d.value = std::numeric_limits<double>::quiet_NaN();
            require(effect(PF_Cmd_RENDER, &in, &out, params.data(), &expressionWorld, nullptr) == PF_Err_BAD_CALLBACK_PARAM,
                "nonfinite expression weight rejected");
            parameters[kExpressionWeightA].u.fs_d.value = 0;
            disposeArb(missingExpression, 117);
            parameters[kExpressionSelection].u.arb_d.value = parameters[kExpressionSelection].u.arb_d.dephault;
            std::vector<std::wstring> expressionFiles;
            for (const auto& file : std::filesystem::recursive_directory_iterator(parent)) {
                const auto name = file.path().filename().wstring();
                if (file.is_regular_file() && name.size() >= 10 &&
                    name.compare(name.size() - 10, 10, L".exp3.json") == 0) expressionFiles.push_back(file.path().wstring());
            }
            std::sort(expressionFiles.begin(), expressionFiles.end());
            if (!expressionFiles.empty()) {
                const auto expressionFirst = hexText(utf8Path(expressionFiles.front()));
                const auto expressionLast = hexText(utf8Path(expressionFiles.back()));
                auto expressionBank = scanText("Live2DExpression1|" + expressionFirst + "|||||||" + expressionLast, 117);
                parameters[kExpressionSelection].u.arb_d.value = expressionBank;
                parameters[kExpressionA].u.pd.value = 8;
                parameters[kExpressionWeightA].u.fs_d.value = 100;
                // B is inactive and has an invalid selector; only A is resolved.
                const auto fullExpression = renderExpression8();
                require(!expressionEqual(noExpression, fullExpression), "active expression changes rendered model pixels");
                parameters[kExpressionWeightA].u.fs_d.value = 50;
                const auto halfExpression = renderExpression8();
                require(!expressionEqual(noExpression, halfExpression) && !expressionEqual(fullExpression, halfExpression),
                    "keyframed expression weight reaches an intermediate appearance");
                parameters[kExpressionWeightA].u.fs_d.value = 1000;
                require(expressionEqual(fullExpression, renderExpression8()), "expression weight above range clamps to one");
                parameters[kExpressionWeightA].u.fs_d.value = 0;
                require(expressionEqual(noExpression, renderExpression8()), "returning expression weight to zero restores original motion output");
                parameters[kExpressionB].u.pd.value = 1;
                parameters[kExpressionWeightB].u.fs_d.value = 100;
                const auto expressionB = renderExpression8();
                parameters[kExpressionWeightB].u.fs_d.value = 0;
                parameters[kExpressionA].u.pd.value = 1;
                parameters[kExpressionWeightA].u.fs_d.value = 100;
                require(expressionEqual(expressionB, renderExpression8()), "expression B uses its independent slot and weight");
                parameters[kExpressionA].u.pd.value = 8;
                parameters[kExpressionWeightA].u.fs_d.value = 35; parameters[kExpressionWeightB].u.fs_d.value = 65;
                const auto expressionPair = renderExpression8();
                parameters[kExpressionWeightA].u.fs_d.value = 100; parameters[kExpressionWeightB].u.fs_d.value = 0;
                require(expressionEqual(fullExpression, renderExpression8()), "expression seek after two-source blend is deterministic");
                parameters[kExpressionWeightA].u.fs_d.value = 35; parameters[kExpressionWeightB].u.fs_d.value = 65;
                require(expressionEqual(expressionPair, renderExpression8()), "two independently weighted expressions repeat exactly");
                parameters[kExpressionWeightA].u.fs_d.value = 100; parameters[kExpressionWeightB].u.fs_d.value = 0;
                std::vector<PF_Pixel16> expression16(expressionWidth * expressionHeight);
                expressionWorld.world_flags = PF_WorldFlag_DEEP;
                expressionWorld.rowbytes = expressionWidth * sizeof(PF_Pixel16);
                expressionWorld.data = reinterpret_cast<PF_PixelPtr>(expression16.data());
                require(effect(PF_Cmd_RENDER, &in, &out, params.data(), &expressionWorld, nullptr) == 0,
                    "expression renders through actual AEX 16-bpc output");
                bool channelsAgree = true;
                const auto expand = [](unsigned char value) { return (value * 32768u + 127u) / 255u; };
                for (size_t i = 0; i < expression16.size(); ++i) {
                    const auto& a = fullExpression[i]; const auto& b = expression16[i];
                    channelsAgree &= b.alpha == expand(a.alpha) && b.red == expand(a.red) && b.green == expand(a.green) && b.blue == expand(a.blue);
                }
                require(channelsAgree, "expression AE8/AE16 color and alpha agree");
                disposeArb(expressionBank, 117);
                parameters[kExpressionSelection].u.arb_d.value = parameters[kExpressionSelection].u.arb_d.dephault;
                std::cout << "PASS: actual AEX expression pixels, independent A/B weights, inactive missing-file skip, weight fades/clamps, deterministic seeking and AE8/AE16 agreement.\n";
            } else {
                std::cout << "SKIP: expression appearance checks need .exp3.json files under the supplied model folder.\n";
            }
            // The actual AEX consumes opaque audio callbacks; even this audio-only
            // layer has no image dimensions/data. The host mock isolates error,
            // time and ownership cases from the renderer's real mouth pixels.
            parameters[kExpressionWeightA].u.fs_d.value = parameters[kExpressionWeightB].u.fs_d.value = 0;
            expressionWorld.world_flags = 0;
            expressionWorld.rowbytes = expressionWidth * sizeof(PF_Pixel8);
            expressionWorld.data = expressionImage.data();
            in.current_time = 120; in.time_scale = 60;
            parameters[kLipSensitivity].u.fs_d.value = std::numeric_limits<double>::quiet_NaN();
            const auto oldAudioDisabled = renderExpression8();
            require(audioMock::checkouts == 0 && !in.inter.checkout_layer_audio,
                "disabled audio bypasses absent callbacks and invalid sensitivity");
            parameters[kLipSensitivity].u.fs_d.value = 100;
            audioMock::install(in);
            parameters[kLipSync].u.bd.value = TRUE;
            require(!parameters[kAudioLayer].u.ld.width && !parameters[kAudioLayer].u.ld.data, "fixture is audio only without pixels");
            const auto audioRender = [&]() {
                return effect(PF_Cmd_RENDER, &in, &out, params.data(), &expressionWorld, nullptr);
            };
            const unsigned beforeNullGet = audioMock::gets;
            const unsigned beforeNullCheckin = audioMock::checkins;
            const auto nullAudioMouth = renderExpression8();
            require(audioMock::gets == beforeNullGet && audioMock::checkins == beforeNullCheckin && !audioMock::live,
                "REGRESSION: enabled None/null audio calls neither get_audio_data nor checkin_layer_audio");
            require(audioMock::lastStart == 40572 && audioMock::lastDuration == 3528 && audioMock::lastScale == 22050,
                "two seconds maps to the absolute preceding 160ms sample window");
            audioMock::provide = true;
            audioMock::tone(0.30);
            const auto openMouth = renderExpression8();
            require(!expressionEqual(oldAudioDisabled, openMouth), "actual AEX maps audio-only PCM samples to visible mouth opening");
            audioMock::tone(0);
            const auto silentMouth = renderExpression8();
            require(!expressionEqual(openMouth, silentMouth), "selected silent audio closes the mouth instead of skipping lip sync");
            require(expressionEqual(nullAudioMouth, oldAudioDisabled),
                "None/null audio preserves the original motion mouth rather than overriding it with silence");
            audioMock::tone(0.30);
            parameters[kLipSensitivity].u.fs_d.value = 0;
            require(expressionEqual(silentMouth, renderExpression8()), "zero sensitivity closes the mouth with active audio");
            parameters[kLipSensitivity].u.fs_d.value = -5;
            require(expressionEqual(silentMouth, renderExpression8()), "sensitivity below range clamps to zero");
            parameters[kLipSensitivity].u.fs_d.value = 100;
            require(expressionEqual(openMouth, renderExpression8()), "restoring sensitivity reproduces mouth pixels without frame history");
            audioMock::returnedFrames = audioMock::frameCount + 1;
            require(expressionEqual(openMouth, renderExpression8()), "optional SDK terminal zero frame does not change analysis");
            audioMock::returnedFrames = 0; audioMock::nullData = true;
            require(expressionEqual(silentMouth, renderExpression8()), "empty selected audio buffer closes mouth without dereferencing null");
            audioMock::returnedFrames = audioMock::frameCount; audioMock::nullData = false;
            parameters[kLipSync].u.bd.value = FALSE;
            const unsigned beforeDisabled = audioMock::checkouts;
            require(expressionEqual(oldAudioDisabled, renderExpression8()) && audioMock::checkouts == beforeDisabled,
                "disabling lip sync restores the original model image and bypasses checkout");
            parameters[kLipSync].u.bd.value = TRUE;

            const auto expectAudioError = [&](PF_Err expected, const char* label) {
                const unsigned before = audioMock::checkouts;
                require(audioRender() == expected, label);
                require(audioMock::checkouts == before + 1 && audioMock::handles == audioMock::checkins && !audioMock::live,
                    "every returned audio resource is checked in exactly once on failure");
            };
            audioMock::checkoutError = PF_Interrupt_CANCEL;
            const auto getsBeforeCancel = audioMock::gets;
            expectAudioError(PF_Interrupt_CANCEL, "audio checkout cancellation propagates unchanged");
            require(audioMock::gets == getsBeforeCancel, "failed checkout does not read audio data");
            audioMock::provide = false;
            const auto checkinsBeforeNullCancel = audioMock::checkins;
            expectAudioError(PF_Interrupt_CANCEL, "cancelled checkout with null handle propagates without cleanup call");
            require(audioMock::checkins == checkinsBeforeNullCancel,
                "cancelled null checkout never passes null into the host checkin callback");
            audioMock::checkoutError = PF_Err_OUT_OF_MEMORY;
            expectAudioError(PF_Err_OUT_OF_MEMORY, "failed null checkout preserves the original host error");
            require(audioMock::checkins == checkinsBeforeNullCancel,
                "failed null checkout has no audio resource to release");
            audioMock::provide = true;
            audioMock::checkoutError = PF_Err_OUT_OF_MEMORY; audioMock::checkinError = PF_Err_BAD_CALLBACK_PARAM;
            expectAudioError(PF_Err_OUT_OF_MEMORY, "original checkout error takes priority over cleanup failure");
            audioMock::checkoutError = 0; audioMock::checkinError = 0; audioMock::getError = PF_Interrupt_CANCEL;
            expectAudioError(PF_Interrupt_CANCEL, "get_audio_data cancellation propagates with cleanup");
            audioMock::getError = 0; audioMock::checkinError = PF_Err_OUT_OF_MEMORY;
            expectAudioError(PF_Err_OUT_OF_MEMORY, "checkin failure propagates after successful analysis");
            audioMock::checkinError = 0;
            const auto badFormat = [&](A_long& field, A_long wrong, const char* label) {
                const auto saved = field; field = wrong;
                expectAudioError(PF_Err_BAD_CALLBACK_PARAM, label); field = saved;
            };
            badFormat(audioMock::returnedFrames, -1, "negative audio frame count rejected");
            badFormat(audioMock::returnedFrames, audioMock::frameCount + 2, "unbounded host audio buffer rejected");
            badFormat(audioMock::returnedBytes, PF_SSS_4, "unexpected PCM width rejected");
            badFormat(audioMock::returnedChannels, PF_Channels_MONO, "unexpected channel count rejected");
            badFormat(audioMock::returnedFormat, PF_UNSIGNED_PCM, "unsigned samples rejected");
            badFormat(audioMock::returnedFormat, PF_SIGNED_FLOAT, "float samples are not misread as PCM16");
            audioMock::returnedRate = 0;
            expectAudioError(PF_Err_BAD_CALLBACK_PARAM, "zero sample rate rejected");
            audioMock::returnedRate = 44100u * 65536u;
            expectAudioError(PF_Err_BAD_CALLBACK_PARAM, "unexpected host resampling rate rejected");
            audioMock::returnedRate = audioMock::fixedRate; audioMock::nullData = true;
            expectAudioError(PF_Err_BAD_CALLBACK_PARAM, "nonnull frame count with null audio data rejected");
            audioMock::nullData = false; audioMock::misaligned = true;
            expectAudioError(PF_Err_BAD_CALLBACK_PARAM, "unaligned PCM16 pointer rejected before access");
            audioMock::misaligned = false;
            const auto expectBeforeCheckout = [&](PF_Err expected, const char* label) {
                const auto count = audioMock::checkouts;
                require(audioRender() == expected && audioMock::checkouts == count && !audioMock::live, label);
            };
            parameters[kLipSensitivity].u.fs_d.value = std::numeric_limits<double>::infinity();
            expectBeforeCheckout(PF_Err_BAD_CALLBACK_PARAM, "nonfinite audio sensitivity rejected before checkout");
            parameters[kLipSensitivity].u.fs_d.value = 100;
            in.time_scale = 0;
            expectBeforeCheckout(PF_Err_BAD_CALLBACK_PARAM, "zero host time scale rejected without division");
            in.time_scale = 1; in.current_time = (std::numeric_limits<A_long>::max)();
            expectBeforeCheckout(PF_Err_BAD_CALLBACK_PARAM, "positive sample-clock overflow rejected before host call");
            in.current_time = (std::numeric_limits<A_long>::min)();
            expectBeforeCheckout(PF_Err_BAD_CALLBACK_PARAM, "negative sample-clock overflow rejected before host call");
            in.time_scale = 60; in.current_time = 0;
            require(audioRender() == 0 && audioMock::lastStart == -3528, "first-frame lookbehind remains negative for host padding");
            in.time_scale = 11; in.current_time = -1;
            require(audioRender() == 0 && audioMock::lastStart == -5533, "negative nonintegral layer time maps to floor sample time");
            in.time_scale = 1; in.current_time = 2;
            require(audioRender() == 0 && audioMock::lastStart == 40572 && audioMock::lastDuration == 3528,
                "coarse host clock retains an exact 160ms window");
            in.time_scale = (std::numeric_limits<A_u_long>::max)();
            in.current_time = (std::numeric_limits<A_long>::max)();
            require(audioRender() == 0 && audioMock::lastStart == 7496, "large rational clock values do not overflow intermediate math");
            in.time_scale = 60; in.current_time = 120;
            in.inter.get_audio_data = nullptr;
            expectBeforeCheckout(PF_Err_INVALID_CALLBACK, "missing host audio callback fails explicitly");
            audioMock::install(in);
            abortError = PF_Interrupt_CANCEL;
            expectBeforeCheckout(PF_Interrupt_CANCEL, "host abort stops audio work before checkout");
            abortError = PF_Err_NONE;
            audioMock::provide = false;
            require(expressionEqual(oldAudioDisabled, renderExpression8()), "enabled empty/undone audio selection preserves motion mouth");
            audioMock::provide = true;
            require(expressionEqual(openMouth, renderExpression8()), "audio render after errors and reverse seeks is deterministic");

            std::vector<PF_Pixel16> mouth16(expressionWidth * expressionHeight);
            expressionWorld.world_flags = PF_WorldFlag_DEEP;
            expressionWorld.rowbytes = expressionWidth * sizeof(PF_Pixel16);
            expressionWorld.data = reinterpret_cast<PF_PixelPtr>(mouth16.data());
            require(audioRender() == 0, "audio mouth renders through actual AEX 16-bpc output");
            bool mouthChannelsAgree = true;
            for (size_t i = 0; i < mouth16.size(); ++i) {
                const auto& a = openMouth[i]; const auto& b = mouth16[i];
                const auto expand = [](unsigned char value) { return (value * 32768u + 127u) / 255u; };
                mouthChannelsAgree &= b.alpha == expand(a.alpha) && b.red == expand(a.red) &&
                    b.green == expand(a.green) && b.blue == expand(a.blue);
            }
            require(mouthChannelsAgree, "audio mouth AE8/AE16 color and alpha agree");
            expressionWorld.world_flags = 0;
            expressionWorld.rowbytes = expressionWidth * sizeof(PF_Pixel8);
            expressionWorld.data = expressionImage.data();
            if (!motions.empty() && !expressionFiles.empty()) {
                auto combinedMotion = scanText("Live2DNative2|" + modelHex + "|" +
                    hexText(utf8Path(motions.front())) + "|||||||" + hexText(utf8Path(motions.back())));
                auto combinedExpression = scanText("Live2DExpression1|" +
                    hexText(utf8Path(expressionFiles.front())) + "|||||||" + hexText(utf8Path(expressionFiles.back())), 117);
                parameters[kSelection].u.arb_d.value = combinedMotion;
                parameters[kExpressionSelection].u.arb_d.value = combinedExpression;
                parameters[kMotionA].u.pd.value = 1; parameters[kMotionB].u.pd.value = 8;
                parameters[kMotionTimeA].u.fs_d.value = 1; parameters[kMotionTimeB].u.fs_d.value = 0.75;
                parameters[kBlend].u.fs_d.value = 37;
                parameters[kExpressionA].u.pd.value = 1; parameters[kExpressionB].u.pd.value = 8;
                parameters[kExpressionWeightA].u.fs_d.value = 35; parameters[kExpressionWeightB].u.fs_d.value = 65;
                parameters[kLipSync].u.bd.value = FALSE;
                const auto originalCombined = renderExpression8();
                parameters[kLipSync].u.bd.value = TRUE;
                const auto voicedCombined = renderExpression8();
                audioMock::tone(0);
                const auto silentCombined = renderExpression8();
                require(!expressionEqual(voicedCombined, silentCombined), "audio mouth coexists with blended motion and expression clips");
                audioMock::provide = false;
                require(expressionEqual(originalCombined, renderExpression8()), "null track data preserves active motion and expression mouth behavior");
                audioMock::provide = true;
                audioMock::tone(0.30);
                parameters[kLipSync].u.bd.value = FALSE;
                require(expressionEqual(originalCombined, renderExpression8()), "disabling audio restores exact motion/expression blend pixels");
                require(parameters[kSelection].u.arb_d.value == combinedMotion && parameters[kExpressionSelection].u.arb_d.value == combinedExpression &&
                    parameters[kBlend].u.fs_d.value == 37 && parameters[kExpressionWeightA].u.fs_d.value == 35 && parameters[kExpressionWeightB].u.fs_d.value == 65,
                    "lip sync does not alter clip banks or evaluated motion/expression controls");
                disposeArb(combinedMotion); disposeArb(combinedExpression, 117);
                parameters[kSelection].u.arb_d.value = expressionModel;
                parameters[kExpressionSelection].u.arb_d.value = parameters[kExpressionSelection].u.arb_d.dephault;
            }
            parameters[kLipSync].u.bd.value = FALSE;
            require(audioMock::handles == audioMock::checkins && !audioMock::live, "no leaked audio resources across success/error/disable cases");
            std::cout << "PASS: native audio selector; PCM16 stereo checkout; exact 160ms negative/coarse/rational time windows; error/cancellation cleanup; real AEX mouth pixels, silence, sensitivity, 8/16-bpc and motion/expression restoration.\n";
            const auto firstMotion = motions.empty() ? std::string{} : hexText(utf8Path(motions.front()));
            const auto lastMotion = motions.empty() ? std::string{} : hexText(utf8Path(motions.back()));
            const auto firstExpression = expressionFiles.empty() ? std::string{} : hexText(utf8Path(expressionFiles.front()));
            const auto lastExpression = expressionFiles.empty() ? std::string{} : hexText(utf8Path(expressionFiles.back()));
            auto concurrentMotion = scanText("Live2DNative2|" + modelHex + "|" + firstMotion + "|||||||" + lastMotion);
            auto concurrentExpression = scanText("Live2DExpression1|" + firstExpression + "|||||||" + lastExpression, 117);
            auto concurrentParameters = parameters;
            concurrentParameters[kSelection].u.arb_d.value = concurrentMotion;
            concurrentParameters[kExpressionSelection].u.arb_d.value = concurrentExpression;
            concurrentParameters[kMotionA].u.pd.value = 1; concurrentParameters[kMotionB].u.pd.value = 8;
            concurrentParameters[kExpressionA].u.pd.value = 1; concurrentParameters[kExpressionB].u.pd.value = 8;
            runConcurrentRenders(effect, in, concurrentParameters);
            disposeArb(concurrentMotion); disposeArb(concurrentExpression, 117);
            disposeArb(expressionModel);
        }
        disposeArb(blank); disposeArb(original); disposeArb(copy); disposeArb(interpolated);
        disposeArb(parameters[kSelection].u.arb_d.dephault);
        disposeArb(parameters[kExpressionSelection].u.arb_d.dephault, 117);
        require(allocations.empty(), "no leaked host handles");
        require(effect(PF_Cmd_GLOBAL_SETDOWN, &in, &out, nullptr, nullptr, nullptr) == 0, "global teardown");
        FreeLibrary(module);
        timelineMock::run();
        std::cout << "PASS: " << checks.load() << " checks; actual AEX loading/registration; 11 callbacks for each independent motion/expression bank; v1/v2 motion migration; eight-slot Unicode save/copy/clipboard; malformed input; 8/16-bpc render; no handle leaks.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
