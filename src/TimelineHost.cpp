#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include "AEConfig.h"
#include "AE_GeneralPlug.h"
#include "SPBasic.h"
#include "TimelineHost.h"
#include "UiText.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace l2dae {
namespace {
template<class T> class Suite {
public:
    Suite(SPBasicSuite* basic, const char* name, int version) : basic_(basic), name_(name), version_(version) {
        const void* value = nullptr;
        if (!basic || !basic->AcquireSuite || !basic->ReleaseSuite ||
            basic->AcquireSuite(name, version, &value) || !value)
            throw std::runtime_error(std::string("After Effects does not provide ") + name + ".");
        value_ = static_cast<const T*>(value);
    }
    ~Suite() { if (value_) basic_->ReleaseSuite(name_, version_); }
    const T* operator->() const noexcept { return value_; }
    const T* get() const noexcept { return value_; }
    Suite(const Suite&) = delete;
    Suite& operator=(const Suite&) = delete;
private:
    SPBasicSuite* basic_;
    const char* name_;
    int version_;
    const T* value_ = nullptr;
};

struct HostState {
    SPBasicSuite* basic = nullptr;
    AEGP_PluginID pluginId = 0;
    DWORD mainThread = 0;
    bool registeredPlugin = false;
    bool registeredIdle = false;
    bool enabled = false;
    bool busy = false;
    unsigned importDepth = 0;
    std::deque<std::string> pending;
};
// No unregister-idle API exists. This state has DLL lifetime and is never freed
// by PF_Cmd_GLOBAL_SETDOWN, even when an effect is disabled and set up again.
HostState host;
int moduleAddress;

void preflight(const AEGP_UtilitySuite6* utility) {
    A_Boolean suppressed = FALSE, scripting = FALSE;
    if (!utility->AEGP_GetSuppressInteractiveUI || !utility->AEGP_IsScriptingAvailable ||
        !utility->AEGP_ExecuteScript ||
        utility->AEGP_GetSuppressInteractiveUI(&suppressed) || suppressed)
        throw std::runtime_error("Timeline clips require an interactive After Effects session.");
    if (utility->AEGP_IsScriptingAvailable(&scripting) || !scripting)
        throw std::runtime_error("After Effects scripting is unavailable. Timeline clips could not be added.");
}

std::string readHelper() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&moduleAddress), &module))
        throw std::runtime_error("Cannot locate the Live2D plugin module.");
    std::vector<wchar_t> path(512);
    for (;;) {
        const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!size) throw std::runtime_error("Cannot read the Live2D plugin installation path.");
        if (size < path.size()) { path.resize(size); break; }
        if (path.size() >= 32768) throw std::runtime_error("Live2D plugin installation path is too long.");
        path.resize((std::min)(path.size() * 2, size_t{32768}));
    }
    const auto helper = std::filesystem::path(std::wstring(path.begin(), path.end())).parent_path() /
        L"scripts" / L"TimelineClips.jsx";
    std::ifstream file(helper, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing scripts/TimelineClips.jsx beside AeGOFlash.aex. Install the complete plugin folder.");
    const auto length = file.tellg();
    if (length <= 0 || length > 2 * 1024 * 1024)
        throw std::runtime_error("TimelineClips.jsx is empty or exceeds the supported size.");
    std::string result(static_cast<size_t>(length), '\0');
    file.seekg(0);
    if (!file.read(result.data(), static_cast<std::streamsize>(result.size())))
        throw std::runtime_error("Cannot read scripts/TimelineClips.jsx.");
    if (result.compare(0, 3, "\xef\xbb\xbf") == 0) result.erase(0, 3);
    if (result.find('\0') != std::string::npos || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            result.data(), static_cast<int>(result.size()), nullptr, 0))
        throw std::runtime_error("TimelineClips.jsx must be a UTF-8 script without embedded NUL bytes.");
    return result;
}

std::string quoteLabel(const std::wstring& label) {
    if (label.size() > 4096 || (!label.empty() && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            label.data(), static_cast<int>(label.size()), nullptr, 0, nullptr, nullptr)))
        throw std::runtime_error("Clip label is too long or contains invalid Unicode.");
    // ASCII-only JSON string: quotes, backslashes, control characters, Unicode
    // line separators, and UTF-16 surrogate pairs cannot escape into script code.
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << '"';
    for (wchar_t c : label) {
        if (c == L'"' || c == L'\\') text << '\\' << static_cast<char>(c);
        else if (c >= 0x20 && c <= 0x7e) text << static_cast<char>(c);
        else text << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
    }
    text << '"';
    return text.str();
}

std::string commandPayload(const TimelineCommand& command) {
    if (command.newBinding <= 0 || command.oldBinding < 0 || command.newBinding == command.oldBinding ||
        command.slot < 1 || command.slot > 16777216 || !std::isfinite(command.duration) ||
        command.duration <= 0 || command.duration > 86400 ||
        !std::isfinite(command.transitionFrames) || std::floor(command.transitionFrames) != command.transitionFrames ||
        command.transitionFrames < 0 || command.transitionFrames > 100000 ||
        (command.transitionCurve < 0 || command.transitionCurve > 5))
        throw std::runtime_error("Invalid Live2D timeline clip request.");
    std::ostringstream payload;
    payload.imbue(std::locale::classic());
    payload << "{\"kind\":\"" << (command.expression ? "expression" : "motion")
        << "\",\"binding\":" << command.newBinding << ",\"previousBinding\":" << command.oldBinding
        << ",\"slot\":" << command.slot << ",\"duration\":" << std::setprecision(17) << command.duration
        << ",\"label\":" << quoteLabel(command.label) << ",\"append\":" << (command.append ? "true" : "false")
        << ",\"transitionFrames\":" << command.transitionFrames
        << ",\"transitionCurve\":" << command.transitionCurve << '}';
    return payload.str();
}

std::string buildScript(const std::vector<TimelineCommand>& commands) {
    if (commands.empty() || commands.size() > 2 ||
        (commands.size() == 2 && commands[0].expression == commands[1].expression))
        throw std::runtime_error("Choose one motion, one expression, or both for this import.");
    std::string payload = "[";
    for (const auto& command : commands) {
        if (payload.size() > 1) payload += ',';
        payload += commandPayload(command);
    }
    return readHelper() + "\n;L2DAE_addClips(" + payload + "]);\n";
}

class ScriptHandle {
public:
    explicit ScriptHandle(const AEGP_MemorySuite1* memory) : memory_(memory) {}
    ~ScriptHandle() { if (value) memory_->AEGP_FreeMemHandle(value); }
    std::string text() const {
        if (!value) return {};
        AEGP_MemSize size = 0;
        if (memory_->AEGP_GetMemHandleSize(value, &size) || size > 2 * 1024 * 1024)
            throw std::runtime_error("Cannot read the After Effects script result.");
        if (!size) return {};
        void* data = nullptr;
        if (memory_->AEGP_LockMemHandle(value, &data) || !data)
            throw std::runtime_error("Cannot lock the After Effects script result.");
        try {
            const char* chars = static_cast<const char*>(data);
            const char* end = std::find(chars, chars + size, '\0');
            std::string result(chars, end);
            memory_->AEGP_UnlockMemHandle(value);
            return result;
        } catch (...) {
            memory_->AEGP_UnlockMemHandle(value);
            throw;
        }
    }
    ScriptHandle(const ScriptHandle&) = delete;
    ScriptHandle& operator=(const ScriptHandle&) = delete;
    AEGP_MemHandle value = nullptr;
private:
    const AEGP_MemorySuite1* memory_;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

void reportFailure(const char* message) noexcept {
    try {
        Suite<AEGP_UtilitySuite6> utility(host.basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
        const std::string text = std::string(u8"无法添加 AeGO Flash 片段。\n") + errorTextUtf8(message ? message : "") +
            u8"\n请检查时间线后，再重新导入。";
        const int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (wideSize > 0 && utility->AEGP_ReportInfoUnicode) {
            std::wstring wide(static_cast<size_t>(wideSize), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                static_cast<int>(text.size()), wide.data(), wideSize);
            utility->AEGP_ReportInfoUnicode(host.pluginId, reinterpret_cast<const A_UTF16Char*>(wide.c_str()));
        } else if (utility->AEGP_ReportInfo) {
            utility->AEGP_ReportInfo(host.pluginId, text.c_str());
        }
    } catch (...) { /* No exception may leave an AEGP callback. */ }
}

A_Err idleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* maxSleep) noexcept {
    if (!host.enabled || host.busy || host.importDepth || host.pending.empty()) return A_Err_NONE;
    host.busy = true;
    // Remove before script evaluation: script execution can enter nested event
    // loops that call idle again. No second invocation can replay this command.
    std::string script = std::move(host.pending.front());
    host.pending.pop_front();
    try {
        Suite<AEGP_UtilitySuite6> utility(host.basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
        Suite<AEGP_MemorySuite1> memory(host.basic, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
        preflight(utility.get());
        ScriptHandle result(memory.get()), error(memory.get());
        const auto status = utility->AEGP_ExecuteScript(host.pluginId, script.c_str(), FALSE, &result.value, &error.value);
        const auto errorText = trim(error.text());
        if (status || !errorText.empty())
            throw std::runtime_error(errorText.empty() ? "After Effects failed to execute TimelineClips.jsx." : errorText);
        const auto resultText = trim(result.text());
        if (resultText != "OK")
            throw std::runtime_error("TimelineClips.jsx did not confirm clip creation. Check the target effect still exists, then import the clip again.");
    } catch (const std::exception& error) {
        reportFailure(error.what());
    } catch (...) {
        reportFailure("Unexpected native timeline bridge error.");
    }
    host.busy = false;
    if (maxSleep && !host.pending.empty()) *maxSleep = (std::min)(*maxSleep, A_long{1});
    return A_Err_NONE;
}

void initialize(SPBasicSuite* basic) {
    if (!basic) throw std::runtime_error("This host does not provide the After Effects timeline scripting bridge.");
    if (host.registeredIdle) {
        if (host.basic != basic || host.mainThread != GetCurrentThreadId())
            throw std::runtime_error("Timeline clips must be imported on the After Effects main thread.");
        host.enabled = true;
        return;
    }
    Suite<AEGP_UtilitySuite6> utility(basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    Suite<AEGP_RegisterSuite5> registration(basic, kAEGPRegisterSuite, kAEGPRegisterSuiteVersion5);
    Suite<AEGP_MemorySuite1> memory(basic, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
    preflight(utility.get());
    if (!utility->AEGP_RegisterWithAEGP || !registration->AEGP_RegisterIdleHook ||
        !memory->AEGP_FreeMemHandle || !memory->AEGP_GetMemHandleSize ||
        !memory->AEGP_LockMemHandle || !memory->AEGP_UnlockMemHandle)
        throw std::runtime_error("Required After Effects timeline callbacks are unavailable.");
    if (!host.registeredPlugin) {
        if (utility->AEGP_RegisterWithAEGP(reinterpret_cast<AEGP_GlobalRefcon>(&host), "AeGO Flash", &host.pluginId))
            throw std::runtime_error("Cannot register the Live2D timeline bridge with After Effects.");
        host.registeredPlugin = true;
        host.basic = basic;
        host.mainThread = GetCurrentThreadId();
    }
    if (registration->AEGP_RegisterIdleHook(host.pluginId, idleHook, reinterpret_cast<AEGP_IdleRefcon>(&host)))
        throw std::runtime_error("Cannot register the Live2D timeline idle callback.");
    host.registeredIdle = true;
    host.enabled = true;
}
} // namespace

bool initializeTimelineHost(SPBasicSuite* basic) noexcept {
    try { initialize(basic); return true; }
    catch (...) { return false; }
}

void requireTimelineHost(SPBasicSuite* basic) {
    initialize(basic);
    Suite<AEGP_UtilitySuite6> utility(host.basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    preflight(utility.get());
}

TimelineImportScope::TimelineImportScope(SPBasicSuite* basic) {
    if (host.mainThread && host.mainThread != GetCurrentThreadId())
        throw std::runtime_error("Live2D imports must run on the After Effects main thread.");
    ++host.importDepth;
    try {
        requireTimelineHost(basic);
        Suite<AEGP_UtilitySuite6> utility(host.basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
        HWND owner = nullptr;
        // AEGP_GetMainHWND takes a void* output address, not an HWND value.
        if (!utility->AEGP_GetMainHWND || utility->AEGP_GetMainHWND(&owner) ||
            !owner || !IsWindow(owner) || GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId())
            throw std::runtime_error("Cannot obtain the After Effects main window. Live2D import was not opened.");
        ownerWindow_ = owner;
    } catch (...) {
        --host.importDepth;
        throw;
    }
}

TimelineImportScope::~TimelineImportScope() noexcept {
    --host.importDepth;
    // Do not call AE or pump messages here: PF still has to commit the updated
    // parameter values after its callback returns. A later idle drains the queue.
}

std::int32_t newTimelineBinding(std::int32_t previous) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create a unique timeline clip binding.");
        std::uint32_t words[4];
        static_assert(sizeof(words) == sizeof(guid));
        std::memcpy(words, &guid, sizeof(words));
        const auto token = static_cast<std::int32_t>((words[0] ^ words[1] ^ words[2] ^ words[3]) & 0x7fffffffu);
        if (token && token != previous) return token;
    }
    throw std::runtime_error("Cannot generate a new timeline clip binding. Try importing again.");
}

void enqueueTimelineCommand(SPBasicSuite* basic, const TimelineCommand& command) {
    enqueueTimelineCommands(basic, {command});
}

void enqueueTimelineCommands(SPBasicSuite* basic, const std::vector<TimelineCommand>& commands) {
    requireTimelineHost(basic);
    auto script = buildScript(commands);
    Suite<AEGP_UtilitySuite6> utility(host.basic, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    if (host.pending.size() >= 32) throw std::runtime_error("Live2D is still adding earlier clips. Wait and try again.");
    host.pending.push_back(std::move(script));
    // Registered idle hooks run naturally; this merely requests an earlier idle.
    // It is explicitly asynchronous and is not necessary for queue correctness.
    if (utility->AEGP_CauseIdleRoutinesToBeCalled) utility->AEGP_CauseIdleRoutinesToBeCalled();
}

void shutdownTimelineHost() noexcept {
    host.enabled = false;
    host.pending.clear();
}
} // namespace l2dae
