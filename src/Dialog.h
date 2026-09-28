#pragma once
#include <string>
#include <vector>

namespace l2dae {
struct RenderRequest;
struct ModelSelection {
    std::wstring modelPath;
    std::vector<std::wstring> motionPaths;
    std::vector<std::wstring> expressionPaths;
};

struct MotionClipSelection {
    ModelSelection selection;
    int slot = 1;
    std::wstring label;
    double duration = 0;
    bool append = true;
    int transitionFrames = 30;
    int transitionCurve = 5; // 3: smooth/no rebound; 4: gentle; 5: pronounced (first use). 0..2 are legacy.
};

struct AnimationImportSelection {
    ModelSelection selection;
    bool hasMotion = false;
    bool hasExpression = false;
    MotionClipSelection motion;
    MotionClipSelection expression;
    int transitionFrames = 30;
    int transitionCurve = 5;
};

// Cancel leaves the caller's selection unchanged. Preview uses a private
// background worker and never invokes an After Effects API.
bool showModelImportDialog(ModelSelection& selection, void* ownerWindow);
bool showAnimationImportDialog(const ModelSelection& current, AnimationImportSelection& result,
    void* ownerWindow, const RenderRequest* previewSettings = nullptr);

// Simple import workflow: the slot is assigned internally, never chosen by the user.
// ownerWindow must be AE's main HWND, supplied by TimelineImportScope.
bool showMotionClipDialog(const ModelSelection& current, MotionClipSelection& result, void* ownerWindow);
bool showExpressionClipDialog(const ModelSelection& current, MotionClipSelection& result, void* ownerWindow);

// Runs only in PF_Cmd_USER_CHANGED_PARAM on AE's UI thread. Cancel leaves
// selection untouched; the caller commits accepted changes to an AE parameter.
bool showModelMotionDialog(ModelSelection& selection, void* ownerWindow);
}
