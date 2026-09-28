// Cold-loads the real AEX as a cached-discovery host. No registration export is
// looked up or called. Each CTest invocation must be a fresh process named
// AfterFX.exe, with an explicitly identified AeGO Flash test-host resource.
// This fixture tests callback/encoding contracts; it is not Adobe After Effects.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectSuites.h"
#include "SPBasic.h"
#include "../src/Plugin.h"

#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef AE_TEST_HOST_MAJOR
#error Define AE_TEST_HOST_MAJOR for this test host and its VERSIONINFO resource.
#endif
#ifndef AE_TEST_HOST_MINOR
#error Define AE_TEST_HOST_MINOR for this test host and its VERSIONINFO resource.
#endif

namespace {
using namespace l2dae;
using Effect = PF_Err(*)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef**, PF_LayerDef*, void*);
unsigned checks = 0;
void require(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error(label);
    ++checks;
}

struct Allocation { size_t size; unsigned locks = 0; };
std::unordered_map<PF_Handle, Allocation> allocations;
std::vector<PF_ParamDef> parameters(1);
unsigned allocated = 0, disposed = 0;
bool failAllocation = false;
PF_Handle allocate(A_u_longlong size) {
    if (failAllocation) throw std::bad_alloc();
    auto** address = new char*(nullptr);
    try { *address = new char[static_cast<size_t>(size)]{}; }
    catch (...) { delete address; throw; }
    auto handle = reinterpret_cast<PF_Handle>(address);
    try { allocations.emplace(handle, Allocation{static_cast<size_t>(size)}); }
    catch (...) { delete[] *address; delete address; throw; }
    ++allocated;
    return handle;
}
void* lock(PF_Handle handle) { ++allocations.at(handle).locks; return *handle; }
void unlock(PF_Handle handle) {
    auto& value = allocations.at(handle);
    require(value.locks != 0, "host handle unlock is balanced");
    --value.locks;
}
void dispose(PF_Handle handle) {
    require(allocations.at(handle).locks == 0, "host handle is unlocked before disposal");
    require(allocations.erase(handle) == 1, "host disposes only its live handles");
    delete[] reinterpret_cast<char*>(*handle);
    delete reinterpret_cast<char**>(handle);
    ++disposed;
}
A_u_longlong sizeOf(PF_Handle handle) { return allocations.at(handle).size; }
PF_Err addParam(PF_ProgPtr, PF_ParamIndex index, PF_ParamDefPtr definition) {
    require(index == -1 && definition, "real plugin appends valid parameter definitions");
    parameters.push_back(*definition);
    if (definition->param_type == PF_Param_ARBITRARY_DATA)
        parameters.back().u.arb_d.value = definition->u.arb_d.dephault;
    if (definition->param_type == PF_Param_FLOAT_SLIDER &&
        (definition->flags & PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS))
        parameters.back().u.fs_d.value = definition->u.fs_d.dephault;
    return PF_Err_NONE;
}
PF_Err abortNone(PF_ProgPtr) { return PF_Err_NONE; }

enum class LanguageState { Chinese, AcquireFailure, CallbackFailure, MissingCallback, Unterminated };
LanguageState languageState = LanguageState::Chinese;
unsigned suiteAcquires = 0, suiteReleases = 0, languageCalls = 0;
int suiteReferences = 0;
PF_Err language(A_char* text) {
    ++languageCalls;
    if (languageState == LanguageState::CallbackFailure) {
        // A failing host may modify its output: that partial result must not
        // replace the last successfully resolved Chinese-language context.
        std::memcpy(text, "en_US", 6);
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    if (languageState == LanguageState::Unterminated) {
        std::memset(text, 'x', PF_APP_LANG_TAG_SIZE);
        return PF_Err_NONE;
    }
    std::memcpy(text, "zh_CN", 6);
    return PF_Err_NONE;
}
PFAppSuite6 appSuite{};
SPErr acquire(const char* name, int32 version, const void** suite) {
    *suite = nullptr;
    if (std::strcmp(name, kPFAppSuite) || version != kPFAppSuiteVersion6 ||
        languageState == LanguageState::AcquireFailure) return 1;
    appSuite.PF_AppGetLanguage = languageState == LanguageState::MissingCallback ? nullptr : language;
    *suite = &appSuite;
    ++suiteAcquires;
    ++suiteReferences;
    return 0;
}
SPErr release(const char* name, int32 version) {
    require(!std::strcmp(name, kPFAppSuite) && version == kPFAppSuiteVersion6 && suiteReferences > 0,
        "language suite release matches a successful acquisition");
    ++suiteReleases;
    --suiteReferences;
    return 0;
}

// This oracle intentionally does not include HostText.h or call production
// encoding/version helpers. The expected policy comes from this fixture's
// independent build identity; PF_InData.version below remains API 13.29.
UINT expectedPage() {
    return AE_TEST_HOST_MAJOR >= 26 || GetOEMCP() == CP_UTF8 ? CP_UTF8 : 936;
}
std::string encode(const wchar_t* text, UINT page) {
    BOOL replaced = FALSE;
    auto* replacement = page == CP_UTF8 ? nullptr : &replaced;
    const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const int count = WideCharToMultiByte(page, flags, text, -1, nullptr, 0, nullptr, replacement);
    require(count > 0 && !replaced, "independent expected text is losslessly encodable");
    std::string result(static_cast<size_t>(count), '\0');
    replaced = FALSE;
    require(WideCharToMultiByte(page, flags, text, -1, result.data(), count, nullptr, replacement) == count && !replaced,
        "independent expected text conversion is lossless");
    result.pop_back();
    return result;
}
std::wstring decode(const char* text, UINT page) {
    const int count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    require(count > 0, "actual AEX text decodes strictly in the cold host's expected code page");
    std::wstring result(static_cast<size_t>(count), L'\0');
    require(MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text, -1, result.data(), count) == count,
        "actual AEX text decoding preserves every character");
    result.pop_back();
    return result;
}
std::string hexPrefix(const char* text) {
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (size_t i = 0; text[i] && i < 24; ++i)
        result << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(text[i])) << ' ';
    return result.str();
}
void expectText(const char* text, const wchar_t* expected, const char* field) {
    require(text != nullptr, std::string(field) + " pointer exists after callback return");
    const auto bytes = encode(expected, expectedPage());
    require(bytes == text, std::string(field) + " encoding mismatch: expected CP" + std::to_string(expectedPage()) +
        " bytes [" + hexPrefix(bytes.c_str()) + "]; actual [" + hexPrefix(text) + "]");
    require(decode(text, expectedPage()) == expected, std::string(field) + " round-trips to exact Chinese text");
}

void validateExecutableIdentity() {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(size > 0 && size < path.size(), "read the real test process path");
    const wchar_t* slash = std::wcsrchr(path.data(), L'\\');
    require(slash && _wcsicmp(slash + 1, L"AfterFX.exe") == 0,
        "cold fixture runs with basename AfterFX.exe in its own test output directory");
    DWORD unused = 0;
    const DWORD byteCount = GetFileVersionInfoSizeW(path.data(), &unused);
    require(byteCount != 0, "test executable contains VERSIONINFO");
    std::vector<unsigned char> data(byteCount);
    require(GetFileVersionInfoW(path.data(), 0, byteCount, data.data()) != FALSE, "read test VERSIONINFO");
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    require(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) &&
        fixed && fixedSize >= sizeof(*fixed) && fixed->dwSignature == 0xFEEF04BD,
        "test executable has valid fixed version information");
    require(HIWORD(fixed->dwProductVersionMS) == AE_TEST_HOST_MAJOR &&
        LOWORD(fixed->dwProductVersionMS) == AE_TEST_HOST_MINOR && fixed->dwProductVersionLS == 0 &&
        fixed->dwFileVersionMS == fixed->dwProductVersionMS && fixed->dwFileVersionLS == 0,
        "resource product/file versions exactly match the independently compiled test case");
    wchar_t* description = nullptr;
    UINT descriptionSize = 0;
    require(VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\FileDescription",
        reinterpret_cast<void**>(&description), &descriptionSize) && descriptionSize > 0 && description &&
        std::wcscmp(description, L"AeGO Flash test host") == 0,
        "VERSIONINFO clearly identifies the executable as an AeGO Flash test host");
}

struct RetainedText { const char* pointer; std::string bytes; };
struct Host {
    HMODULE module = nullptr;
    Effect effect = nullptr;
    PF_UtilCallbacks utils{};
    SPBasicSuite basic{};
    PF_InData input{};
    std::vector<RetainedText> retained;
    bool stopped = false;
    Host() {
        utils.host_new_handle = allocate;
        utils.host_lock_handle = lock;
        utils.host_unlock_handle = unlock;
        utils.host_dispose_handle = dispose;
        utils.host_get_handle_size = sizeOf;
        basic.AcquireSuite = acquire;
        basic.ReleaseSuite = release;
        input.utils = &utils;
        input.inter.add_param = addParam;
        input.inter.abort = abortNone;
        input.pica_basicP = &basic;
        input.version.major = 13;
        input.version.minor = 29;
    }
    ~Host() {
        try { finish(); }
        catch (const std::exception& error) { std::cerr << "CLEANUP FAILED: " << error.what() << '\n'; }
        if (module) FreeLibrary(module);
    }
    void open(const wchar_t* path) {
        module = LoadLibraryW(path);
        require(module != nullptr, "cold LoadLibrary loads the actual AEX");
        // Deliberately the only GetProcAddress in this source. Discovery and
        // both PluginDataEntryFunction variants must remain completely unused.
        effect = reinterpret_cast<Effect>(GetProcAddress(module, "EffectMain"));
        require(effect != nullptr, "cold-loaded actual AEX exports EffectMain");
    }
    void global() {
        PF_OutData out{};
        require(effect(PF_Cmd_GLOBAL_SETUP, &input, &out, nullptr, nullptr, nullptr) == PF_Err_NONE,
            "GLOBAL_SETUP accepts the cold host without discovery");
        require(suiteReferences == 0, "GLOBAL_SETUP releases its language suite before returning");
    }
    void about() {
        PF_OutData out{};
        require(effect(PF_Cmd_ABOUT, &input, &out, nullptr, nullptr, nullptr) == PF_Err_NONE,
            "ABOUT succeeds without depending on registration or GLOBAL_SETUP");
        require(std::memchr(out.return_msg, 0, sizeof(out.return_msg)) != nullptr,
            "About message terminates inside its host buffer");
        const auto expectedBody = encode(L"选择音频图层，启用声音同步口型。", expectedPage());
        require(std::strstr(out.return_msg, expectedBody.c_str()) != nullptr,
            "About Chinese body has the cold host's expected encoding before any version assertion");
        const auto text = decode(out.return_msg, expectedPage());
        require(text.find(L"选择音频图层，启用声音同步口型。") != std::wstring::npos,
            "About Chinese body round-trips without corruption");
        require(text.find(L"AeGO Flash 1.0.2\r") == 0, "About identifies the release as AeGO Flash 1.0.2");
        require(suiteReferences == 0, "ABOUT does not retain host suites");
    }
    void releaseParameters() {
        if (!effect) return;
        for (auto& parameter : parameters) {
            if (parameter.param_type != PF_Param_ARBITRARY_DATA || !parameter.u.arb_d.dephault) continue;
            PF_ArbParamsExtra extra{};
            extra.id = static_cast<A_short>(parameter.uu.id);
            extra.which_function = PF_Arbitrary_DISPOSE_FUNC;
            extra.u.dispose_func_params.arbH = parameter.u.arb_d.dephault;
            PF_OutData out{};
            require(effect(PF_Cmd_ARBITRARY_CALLBACK, &input, &out, nullptr, nullptr, &extra) == PF_Err_NONE,
                "actual AEX releases each host-owned default bank");
            parameter.u.arb_d.dephault = nullptr;
            parameter.u.arb_d.value = nullptr;
        }
        parameters.assign(1, PF_ParamDef{});
        require(allocations.empty(), "all arbitrary parameter handles were released");
    }
    void remember(const char* pointer, const wchar_t* expected, const char* kind) {
        expectText(pointer, expected, kind);
        retained.push_back({pointer, pointer});
    }
    void retainedValid() {
        for (const auto& text : retained)
            require(text.pointer && text.bytes == text.pointer,
                "retained button/checkbox/popup pointers survive setup repetition and failed language discovery");
    }
    void params() {
        require(parameters.size() == 1 && allocations.empty(), "parameter setup starts with clean fake-host storage");
        PF_OutData out{};
        require(effect(PF_Cmd_PARAMS_SETUP, &input, &out, nullptr, nullptr, nullptr) == PF_Err_NONE,
            "PARAMS_SETUP accepts the cold host without discovery");
        require(parameters.size() == kParameterCount && out.num_params == kParameterCount,
            "real AEX creates every expected persistent parameter and group");
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
            {kAudioGroupStart,L"声音与口型"}, {kAmbientGroupStart,L"呼吸与眨眼"}
        };
        for (const auto& expected : labels) {
            const auto& parameter = parameters[expected.index];
            require(std::memchr(parameter.PF_DEF_NAME, 0, sizeof(parameter.PF_DEF_NAME)) != nullptr,
                "parameter label terminates inside its fixed AE buffer");
            expectText(parameter.PF_DEF_NAME, expected.text, "parameter/group label");
            require(parameter.uu.id == kParameterDiskIds[expected.index], "localized parameter preserves its saved disk ID");
        }
        remember(parameters[kOpenDialog].u.button_d.u.namesptr, L"导入模型…", "model button");
        remember(parameters[kImportExpression].u.button_d.u.namesptr, L"导入动作与表情…", "animation button");
        for (auto index : {kLoop, kManualTime, kLipSync, kBreathing, kAutoBlink})
            remember(parameters[index].u.bd.u.nameptr, L"启用", "checkbox");
        for (auto index : {kMotionA, kMotionB, kExpressionA, kExpressionB})
            remember(parameters[index].u.pd.u.namesptr,
                L"槽位 1|槽位 2|槽位 3|槽位 4|槽位 5|槽位 6|槽位 7|槽位 8", "popup");
        require(suiteReferences == 0, "PARAMS_SETUP releases every acquired language suite");
        releaseParameters();
        retainedValid();
    }
    void localizedError() {
        require(parameters.size() == 1 && allocations.empty(), "error probe starts without live parameter banks");
        PF_OutData out{};
        failAllocation = true;
        PF_Err error = PF_Err_NONE;
        try { error = effect(PF_Cmd_PARAMS_SETUP, &input, &out, nullptr, nullptr, nullptr); }
        catch (...) { failAllocation = false; throw; }
        failAllocation = false;
        require(error == PF_Err_OUT_OF_MEMORY && (out.out_flags & PF_OutFlag_DISPLAY_ERROR_MESSAGE),
            "real AEX returns a localized host-visible allocation error");
        require(std::memchr(out.return_msg, 0, sizeof(out.return_msg)) != nullptr, "error fits inside AE's message field");
        expectText(out.return_msg, L"AeGO Flash: 可用内存不足。请降低合成尺寸或释放内存后重试。", "real allocation error");
        releaseParameters();
        require(suiteReferences == 0, "localized error does not leak a language suite");
    }
    void finish() {
        if (stopped || !effect) return;
        failAllocation = false;
        releaseParameters();
        retainedValid();
        PF_OutData out{};
        require(effect(PF_Cmd_GLOBAL_SETDOWN, &input, &out, nullptr, nullptr, nullptr) == PF_Err_NONE,
            "actual AEX global teardown succeeds");
        require(allocations.empty() && allocated == disposed, "cold host test leaks no arbitrary-data handles");
        require(suiteReferences == 0 && suiteAcquires == suiteReleases, "cold host test leaks no acquired suites");
        stopped = true;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3, "usage: AfterFX.exe path/to/AeGOFlash.aex global|params|about");
        const std::wstring mode = argv[2];
        require(mode == L"global" || mode == L"params" || mode == L"about", "first-selector mode is global, params or about");
        validateExecutableIdentity();
        std::cout << "Cold host AE " << AE_TEST_HOST_MAJOR << '.' << AE_TEST_HOST_MINOR
            << ", CP" << expectedPage() << ", PF API 13.29, first selector "
            << (mode == L"global" ? "GLOBAL_SETUP" : mode == L"params" ? "PARAMS_SETUP" : "ABOUT")
            << "; registration is never looked up or invoked.\n";
        Host host;
        host.open(argv[1]);
        if (mode == L"about") host.about();
        if (mode == L"global") host.global();
        host.params();
        if (mode != L"global") host.global();
        host.about();
        host.localizedError();
        require(languageCalls > 0, "real AEX queries the mocked Chinese application language on its cold path");
        for (const auto failure : {LanguageState::AcquireFailure, LanguageState::CallbackFailure,
                LanguageState::MissingCallback, LanguageState::Unterminated}) {
            languageState = failure;
            host.global();
            host.params();
            host.about();
            host.localizedError();
        }
        languageState = LanguageState::Chinese;
        host.global();
        host.params();
        host.about();
        host.finish();
        std::cout << "PASS: " << checks << " checks; all Chinese parameter/group labels, buttons, checkboxes, popups, "
            "About and real error bytes; retained text pointers; " << allocated << '/' << disposed
            << " handles and " << suiteAcquires << '/' << suiteReleases << " suites balanced. "
            "This is a versioned AeGO Flash test process, not an actual Adobe host.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (after " << checks << " checks; "
            << allocations.size() << " live handles, " << suiteReferences << " live suites)\n";
        return 1;
    }
}
