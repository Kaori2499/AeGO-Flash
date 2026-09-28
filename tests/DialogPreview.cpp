// Optional manual acceptance harness for the actual native dialog and renderer.
// It opens its own owner window, never loads AE, and writes only the requested log.
#include <windows.h>
#include "Dialog.h"
#include "Renderer.h"
#include "ImportPreferences.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::cerr << "Usage: DialogPreview model3.json [result.log] [isolated-preferences.ini]\n"; return 2; }
    // This standalone EXE emulates AE's per-monitor DPI behavior. The plugin
    // must never change the host process/thread's DPI awareness globally.
    using SetAwareness = BOOL(WINAPI*)(HANDLE);
    if (const auto setAwareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        setAwareness(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))); // PER_MONITOR_AWARE_V2
    const HWND owner = CreateWindowExW(0, L"STATIC", L"AeGO Flash Preview Test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return 3;
    ShowWindow(owner, SW_SHOW);
    int status = 0;
    try {
        l2dae::ModelSelection current; current.modelPath = std::filesystem::absolute(argv[1]).wstring();
        l2dae::AnimationImportSelection result;
        // Never touch the user's application preference in this test harness.
        l2dae::ImportPreferences preferences(argc > 3 ? std::filesystem::absolute(argv[3]) : std::filesystem::path{});
        result.transitionCurve = preferences.transitionCurve();
        const int initialCurve = result.transitionCurve;
        l2dae::RenderRequest settings; settings.breathingEnabled = settings.autoBlinkEnabled = true;
        const bool accepted = l2dae::showAnimationImportDialog(current, result, owner, &settings);
        const bool saved = argc > 3 && accepted && result.hasMotion && preferences.saveTransitionCurve(result.transitionCurve);
        std::ofstream log(argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path(L"DialogPreview-result.log"));
        log << "accepted=" << accepted << " motion=" << result.hasMotion << " expression=" << result.hasExpression
            << " transitionFrames=" << result.transitionFrames << " motionSlot=" << result.motion.slot
            << " expressionSlot=" << result.expression.slot << " duration=" << result.motion.duration
            << " transitionCurve=" << result.transitionCurve << '\n';
        log << "initialCurve=" << initialCurve << " preferenceSaved=" << saved << '\n';
        for (const auto& path : result.selection.motionPaths) log << "motion=" << std::filesystem::path(path).u8string() << '\n';
        for (const auto& path : result.selection.expressionPaths) log << "expression=" << std::filesystem::path(path).u8string() << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    DestroyWindow(owner);
    l2dae::releaseRenderer();
    return status;
}
