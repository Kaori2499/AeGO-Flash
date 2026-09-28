// Standalone host-contract test. Uses real hidden Win32 owner windows and mocked
// official AEGP suites; it never launches AE, opens a dialog, or executes JSX.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "AEConfig.h"
#include "AE_GeneralPlug.h"
#include "SPBasic.h"
#include "../src/TimelineHost.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {
unsigned checks = 0, acquired = 0, locks = 0, executions = 0, requests = 0;
unsigned registrations = 0, idleRegistrations = 0, ownerCalls = 0;
AEGP_UtilitySuite6 utility{};
AEGP_RegisterSuite5 registration{};
AEGP_MemorySuite1 memory{};
SPBasicSuite basic{};
AEGP_GlobalRefcon globalRefcon = nullptr;
AEGP_IdleRefcon idleRefcon = nullptr;
AEGP_IdleHook idle = nullptr;
HWND owner = nullptr;
bool ownerError = false, scripting = true, preflightIdle = false;
std::string lastScript;
std::unordered_map<AEGP_MemHandle, std::string*> handles;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

void tick() {
    A_long sleep = 60;
    require(idle && idle(globalRefcon, idleRefcon, &sleep) == 0, "idle callback returns without throwing");
}

SPErr acquire(const char* name, int32 version, const void** output) {
    *output = nullptr;
    if (!std::strcmp(name, kAEGPUtilitySuite) && version == kAEGPUtilitySuiteVersion6) *output = &utility;
    if (!std::strcmp(name, kAEGPRegisterSuite) && version == kAEGPRegisterSuiteVersion5) *output = &registration;
    if (!std::strcmp(name, kAEGPMemorySuite) && version == kAEGPMemorySuiteVersion1) *output = &memory;
    if (!*output) return 1;
    ++acquired;
    return 0;
}
SPErr release(const char*, int32) { require(acquired > 0, "balanced suite release"); --acquired; return 0; }
A_Err suppressed(A_Boolean* output) { *output = FALSE; return 0; }
A_Err available(A_Boolean* output) {
    if (preflightIdle) tick();
    *output = scripting;
    return 0;
}
A_Err registerPlugin(AEGP_GlobalRefcon refcon, const A_char*, AEGP_PluginID* output) {
    ++registrations; globalRefcon = refcon; *output = 41; return 0;
}
A_Err registerIdle(AEGP_PluginID id, AEGP_IdleHook hook, AEGP_IdleRefcon refcon) {
    require(id == 41 && hook, "idle registration uses own plugin id");
    ++idleRegistrations; idle = hook; idleRefcon = refcon; return 0;
}
A_Err requestIdle() { ++requests; return 0; }
A_Err mainWindow(void* output) {
    require(output != nullptr, "AE main HWND has an output address");
    ++ownerCalls;
    *static_cast<HWND*>(output) = owner;
    return ownerError ? 1 : 0;
}
AEGP_MemHandle makeHandle(const char* text) {
    auto* value = new std::string(text);
    auto handle = reinterpret_cast<AEGP_MemHandle>(value);
    handles.emplace(handle, value);
    return handle;
}
A_Err size(AEGP_MemHandle handle, AEGP_MemSize* output) {
    *output = static_cast<AEGP_MemSize>(handles.at(handle)->size() + 1); return 0;
}
A_Err lock(AEGP_MemHandle handle, void** output) { *output = handles.at(handle)->data(); ++locks; return 0; }
A_Err unlock(AEGP_MemHandle) { require(locks > 0, "balanced script handle lock"); --locks; return 0; }
A_Err freeHandle(AEGP_MemHandle handle) { delete handles.at(handle); handles.erase(handle); return 0; }
A_Err execute(AEGP_PluginID id, const char* script, A_Boolean encoding,
              AEGP_MemHandle* result, AEGP_MemHandle* error) {
    require(id == 41 && !encoding && script, "valid deferred UTF8 execution");
    lastScript = script;
    ++executions;
    const unsigned before = executions;
    tick();
    require(executions == before, "busy guard still blocks nested idle during script execution");
    *result = makeHandle("OK");
    *error = makeHandle("");
    return 0;
}
void clean() {
    require(acquired == 0 && locks == 0 && handles.empty(), "no suite or script handle leaks");
}

void run() {
    basic.AcquireSuite = acquire; basic.ReleaseSuite = release;
    utility.AEGP_GetSuppressInteractiveUI = suppressed;
    utility.AEGP_IsScriptingAvailable = available;
    utility.AEGP_RegisterWithAEGP = registerPlugin;
    utility.AEGP_GetMainHWND = mainWindow;
    utility.AEGP_CauseIdleRoutinesToBeCalled = requestIdle;
    utility.AEGP_ExecuteScript = execute;
    registration.AEGP_RegisterIdleHook = registerIdle;
    memory.AEGP_GetMemHandleSize = size; memory.AEGP_LockMemHandle = lock;
    memory.AEGP_UnlockMemHandle = unlock; memory.AEGP_FreeMemHandle = freeHandle;

    require(l2dae::initializeTimelineHost(&basic), "bridge initialization succeeds");
    owner = CreateWindowExW(0, L"STATIC", L"Hidden AE owner fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(owner && !IsWindowVisible(owner), "owner fixture remains hidden");
    const HWND validOwner = owner;
    l2dae::TimelineCommand command;
    command.oldBinding = 1; command.newBinding = 2; command.duration = 1;
    command.label = L"Guard fixture";
    l2dae::enqueueTimelineCommand(&basic, command);
    {
        preflightIdle = true;
        l2dae::TimelineImportScope importing(&basic);
        preflightIdle = false;
        require(importing.ownerWindow() == owner, "scope returns the exact AE-provided main HWND");
        require(executions == 0, "scope guards even reentrant preflight host calls");
        tick();
        require(executions == 0, "pending earlier import cannot run in native modal loop");
        {
            l2dae::TimelineImportScope nested(&basic);
            tick();
            require(executions == 0, "nested import remains guarded");
        }
        tick();
        require(executions == 0, "nested destructor cannot release outer import guard");
        l2dae::enqueueTimelineCommand(&basic, command);
        tick();
        require(executions == 0, "newly queued command waits for parameter commit too");
    }
    const unsigned requestedBeforeIdle = requests;
    require(executions == 0, "scope destructor never executes pending work");
    tick();
    require(executions == 1, "first idle after commit executes one command");
    tick();
    require(executions == 2, "second command remains FIFO and executes once");
    require(requests == requestedBeforeIdle, "leaving scope does not call AE to request another nested loop");
    clean();

    l2dae::enqueueTimelineCommand(&basic, command);
    rejects([&] { l2dae::TimelineImportScope importing(&basic); throw std::runtime_error("cancel/error"); },
        "import failure unwinds the scope");
    tick();
    require(executions == 3, "exception cleanup restores ordinary idle processing");

    l2dae::enqueueTimelineCommand(&basic, command);
    {
        l2dae::TimelineImportScope importing(&basic);
        ownerError = true;
        rejects([&] { l2dae::TimelineImportScope failed(&basic); }, "host HWND error rejects a nested dialog");
        ownerError = false;
        tick();
        require(executions == 3, "failed nested constructor preserves outer guard");
    }
    tick();
    require(executions == 4, "failed nested constructor does not leave stale depth");

    l2dae::enqueueTimelineCommand(&basic, command);
    owner = nullptr;
    rejects([&] { l2dae::TimelineImportScope failed(&basic); }, "null AE main HWND is rejected");
    utility.AEGP_GetMainHWND = nullptr;
    rejects([&] { l2dae::TimelineImportScope failed(&basic); }, "missing main HWND callback is rejected");
    utility.AEGP_GetMainHWND = mainWindow;
    owner = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
    rejects([&] { l2dae::TimelineImportScope failed(&basic); }, "invalid HWND is rejected without a fallback window");
    owner = validOwner;
    scripting = false;
    rejects([&] { l2dae::TimelineImportScope failed(&basic); }, "scripting preflight failure releases guard");
    scripting = true;
    tick();
    require(executions == 5, "all constructor errors leave idle enabled and depth balanced");
    clean();

    const unsigned beforeWrongThread = ownerCalls;
    bool threadRejected = false;
    std::thread worker([&] {
        try { l2dae::TimelineImportScope wrongThread(&basic); }
        catch (const std::exception&) { threadRejected = true; }
    });
    worker.join();
    require(threadRejected && ownerCalls == beforeWrongThread, "foreign thread rejected before calling any AE window API");

    {
        l2dae::TimelineImportScope importing(&basic);
        l2dae::enqueueTimelineCommand(&basic, command);
        l2dae::shutdownTimelineHost();
        require(l2dae::initializeTimelineHost(&basic), "bridge can be enabled after teardown");
        l2dae::enqueueTimelineCommand(&basic, command);
        tick();
        require(executions == 5, "teardown does not reset a still-live import scope depth");
    }
    tick();
    require(executions == 6, "teardown discards old queue and later queue survives until scope exits");
    require(registrations == 1 && idleRegistrations == 1, "guards do not duplicate persistent registration");
    clean();

    l2dae::TimelineCommand motion = command, expression = command;
    motion.slot = 41; motion.transitionFrames = 15;
    motion.label = L"Name \"quoted\" \\ \n 中文.motion3.json";
    expression.expression = true; expression.oldBinding = 3; expression.newBinding = 4;
    expression.slot = 100; expression.transitionFrames = 0;
    const auto beforeBatch = executions;
    const auto requestsBeforeBatch = requests;
    l2dae::enqueueTimelineCommands(&basic, {motion, expression});
    require(executions == beforeBatch && requests == requestsBeforeBatch + 1, "a combined batch enqueues one deferred operation");
    tick();
    require(executions == beforeBatch + 1, "a combined batch executes once");
    require(lastScript.find(";L2DAE_addClips([{") != std::string::npos &&
        lastScript.find("\"kind\":\"motion\"") != std::string::npos &&
        lastScript.find("\"kind\":\"expression\"") != std::string::npos,
        "one atomic payload contains both import kinds");
    require(lastScript.find("\"slot\":41") != std::string::npos && lastScript.find("\"slot\":100") != std::string::npos,
        "dynamic bank IDs exceed the legacy eight slots");
    require(lastScript.find("\"transitionFrames\":15") != std::string::npos &&
        lastScript.find("\"transitionFrames\":0") != std::string::npos,
        "default and instant transition settings survive serialization");
    require(lastScript.find("\\\"quoted\\\"") != std::string::npos &&
        lastScript.find("\\u000a") != std::string::npos && lastScript.find("\\u4e2d\\u6587") != std::string::npos,
        "import labels cannot escape the JSON command");
    tick();
    require(executions == beforeBatch + 1, "one batch cannot leave a second deferred command behind");

    const auto beforeInvalid = executions;
    const auto requestsBeforeInvalid = requests;
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {}); }, "empty batch rejected");
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {motion, motion}); }, "duplicate selected channel rejected");
    expression.slot = 16777217;
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {motion, expression}); }, "unsafe float slot index rejected atomically");
    expression.slot = 16777216;
    expression.transitionFrames = -1;
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {motion, expression}); }, "negative transition rejected atomically");
    expression.transitionFrames = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {motion, expression}); }, "NaN transition rejected atomically");
    expression.transitionFrames = 0.5;
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {motion, expression}); }, "fractional transition frames rejected atomically");
    expression.transitionFrames = 100001;
    rejects([&] { l2dae::enqueueTimelineCommands(&basic, {motion, expression}); }, "transition bound matches the import UI");
    tick();
    require(executions == beforeInvalid && requests == requestsBeforeInvalid, "failed batch validation leaves no first-channel operation queued");
    expression.transitionFrames = 15;
    l2dae::enqueueTimelineCommand(&basic, expression);
    tick();
    require(lastScript.find("\"slot\":16777216") != std::string::npos,
        "single-command wrapper accepts the last exactly representable consecutive float ID");
    clean();
    l2dae::shutdownTimelineHost();
    DestroyWindow(owner);
    owner = nullptr;
}
} // namespace

int main() {
    try {
        run();
        std::cout << "PASS: " << checks << " timeline import guard checks; AE owner, modal/nested idle, failures, thread and teardown.\n";
        return 0;
    } catch (const std::exception& error) {
        if (owner && IsWindow(owner)) DestroyWindow(owner);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
