#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SPBasicSuite;

namespace l2dae {
struct TimelineCommand {
    std::int32_t oldBinding = 0;
    std::int32_t newBinding = 0;
    int slot = 1;
    double duration = 0;
    std::wstring label;
    bool append = true;
    bool expression = false;
    double transitionFrames = 30;
    int transitionCurve = 0; // Legacy callers keep uniform transitions.
};

// All calls run on AE's main/UI thread. Only owned strings and numeric values
// enter the queue; PF callback pointers and parameter handles never survive it.
// Missing AEGP suites are harmless during setup, e.g. in an offline render host.
bool initializeTimelineHost(SPBasicSuite* basic) noexcept;
void requireTimelineHost(SPBasicSuite* basic);

// Keep this scope alive from the start of a user import through parameter
// commit. Native modal dialogs pump messages, so an older queued import must
// not run from an idle hook while the current PF parameter array is in use.
// The owner comes from AE, never from whichever floating window is active.
class TimelineImportScope {
public:
    explicit TimelineImportScope(SPBasicSuite* basic);
    ~TimelineImportScope() noexcept;
    TimelineImportScope(const TimelineImportScope&) = delete;
    TimelineImportScope& operator=(const TimelineImportScope&) = delete;
    void* ownerWindow() const noexcept { return ownerWindow_; }
private:
    void* ownerWindow_ = nullptr;
};

std::int32_t newTimelineBinding(std::int32_t previous);
// Validates and loads our module-relative helper before adding a command. The
// actual project operation runs later in AE's idle hook, after PF values commit.
void enqueueTimelineCommand(SPBasicSuite* basic, const TimelineCommand& command);
// A motion and an expression selected together are one transaction and share
// the motion's computed insertion time. No command is queued if validation fails.
void enqueueTimelineCommands(SPBasicSuite* basic, const std::vector<TimelineCommand>& commands);
void shutdownTimelineHost() noexcept;
}
