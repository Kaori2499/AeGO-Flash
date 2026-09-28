#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include "Dialog.h"
#include "Renderer.h"
#include "UiText.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace l2dae {
namespace {
constexpr int kModelPath = 101;
constexpr int kMotionList = 103;
constexpr int kBrowseMotion = 104;
constexpr int kStatus = 106;
constexpr int kSearchMotion = 107;
constexpr int kAppendClip = 110;
constexpr int kExpressionList = 111;
constexpr int kSearchExpression = 112;
constexpr int kBrowseExpression = 113;
constexpr int kPreview = 114;
constexpr int kPlay = 115;
constexpr int kScrub = 116;
constexpr int kTime = 117;
constexpr int kTransition = 118;
constexpr int kMotionPath = 119;
constexpr int kExpressionPath = 120;
constexpr int kUpperBody = 121;
constexpr int kFullBody = 122;
constexpr int kAtPlayhead = 123;
constexpr int kMotionCount = 124;
constexpr int kExpressionCount = 125;
constexpr int kHeading = 126;
constexpr int kSubtitle = 127;
constexpr int kImportSummary = 128;
constexpr int kMotionSearchHint = 129;
constexpr int kExpressionSearchHint = 130;
constexpr int kBounceNone = 131;
constexpr int kBounceGentle = 132;
constexpr int kBounceStrong = 133;
constexpr int kTransitionTitle = 134;
constexpr int kBounceTitle = 135;
constexpr int kPlacementTitle = 136;
constexpr int kTransitionUnit = 137;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT kSeekPreview = WM_APP + 29;
constexpr int kMaximumTransitionFrames = 100000;
constexpr int kLayoutWidth = 1040;
constexpr int kLayoutHeight = 700;
constexpr COLORREF kBackgroundColor = RGB(22, 24, 30);
constexpr COLORREF kCardColor = RGB(31, 34, 42);
constexpr COLORREF kInputColor = RGB(39, 43, 53);
constexpr COLORREF kBorderColor = RGB(57, 64, 79);
constexpr COLORREF kTextColor = RGB(234, 238, 248);
constexpr COLORREF kMutedColor = RGB(156, 167, 186);
constexpr COLORREF kAccentColor = RGB(145, 128, 255);

struct PreviewBounds { int left = 0, top = 0, right = 0, bottom = 0; bool valid = false; };

PreviewBounds alphaBounds(const RenderResult& frame) {
    PreviewBounds bounds{frame.width, frame.height, 0, 0, false};
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.size() != static_cast<size_t>(frame.width) * frame.height * 4) return {};
    for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
        if (frame.rgba[(static_cast<size_t>(y) * frame.width + x) * 4 + 3] < 16) continue;
        bounds.left = std::min(bounds.left, x); bounds.top = std::min(bounds.top, y);
        bounds.right = std::max(bounds.right, x + 1); bounds.bottom = std::max(bounds.bottom, y + 1); bounds.valid = true;
    }
    return bounds;
}

// Bounds are measured once from the neutral character, not each moving frame.
// In renderer coordinates positive offsetY moves the character down on screen.
RenderRequest framePreview(RenderRequest request, const PreviewBounds& bounds, bool upperBody) {
    request.scale = 1; request.offsetX = request.offsetY = 0;
    if (!bounds.valid) return request;
    const float width = static_cast<float>(request.width), height = static_cast<float>(request.height);
    const float characterWidth = static_cast<float>(bounds.right - bounds.left);
    const float characterHeight = static_cast<float>(bounds.bottom - bounds.top);
    const float scale = upperBody ? height * .92f / (characterHeight * .46f)
        : std::min(width * .90f / characterWidth, height * .92f / characterHeight);
    request.scale = std::clamp(scale, .1f, 12.0f);
    request.offsetX = (width * .5f - (bounds.left + bounds.right) * .5f) * request.scale;
    request.offsetY = upperBody
        ? height * .04f - height * .5f - (bounds.top - height * .5f) * request.scale
        : (height * .5f - (bounds.top + bounds.bottom) * .5f) * request.scale;
    return request;
}

int fittedDpi(int desired, int availableWidth, int availableHeight) {
    return std::max(60, std::min({desired, (availableWidth - std::max(32, MulDiv(20, desired, 96))) * 96 / kLayoutWidth,
        (availableHeight - std::max(56, MulDiv(48, desired, 96))) * 96 / kLayoutHeight}));
}

std::wstring fromUtf8(const char* value) {
    return errorTextWide(value ? value : "");
}

bool hasSuffix(const std::wstring& value, const wchar_t* suffix) {
    const size_t count = wcslen(suffix);
    return value.size() >= count && _wcsicmp(value.c_str() + value.size() - count, suffix) == 0;
}

std::wstring animationName(const std::wstring& path) {
    auto name = std::filesystem::path(path).filename().wstring();
    for (const auto* suffix : {L".motion3.json", L".exp3.json"}) {
        if (hasSuffix(name, suffix)) { name.resize(name.size() - wcslen(suffix)); break; }
    }
    return name;
}

HWND checkedOwner(void* ownerWindow) {
    const auto owner = static_cast<HWND>(ownerWindow);
    if (!owner || !IsWindow(owner) || GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId())
        throw std::runtime_error("A valid After Effects main window is required to open Live2D import.");
    return owner;
}

bool browseFile(HWND owner, bool model, std::wstring& path, bool expression = false) {
    std::vector<wchar_t> buffer(32768, L'\0');
    if (path.size() < buffer.size()) std::copy(path.begin(), path.end(), buffer.begin());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = model
        ? uiText(L"Live2D model (*.model3.json)\0*.model3.json\0\0",
            L"Live2D 模型 (*.model3.json)\0*.model3.json\0\0")
        : expression ? uiText(L"Live2D expression (*.exp3.json)\0*.exp3.json\0\0",
            L"Live2D 表情 (*.exp3.json)\0*.exp3.json\0\0")
        : uiText(L"Live2D motion (*.motion3.json)\0*.motion3.json\0\0",
            L"Live2D 动作 (*.motion3.json)\0*.motion3.json\0\0");
    ofn.lpstrTitle = model ? uiText(L"AeGO Flash - Import Model", L"AeGO Flash — 导入模型")
        : expression ? uiText(L"AeGO Flash - External Expression", L"AeGO Flash — 选择外部表情")
        : uiText(L"AeGO Flash - External Motion", L"AeGO Flash — 选择外部动作");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        const auto code = CommDlgExtendedError();
        if (code) throw std::runtime_error("Windows could not open the file picker: " + std::to_string(code));
        return false;
    }
    std::wstring chosen(buffer.data());
    if (!hasSuffix(chosen, model ? L".model3.json" : expression ? L".exp3.json" : L".motion3.json"))
        throw std::runtime_error(model ? "Please choose a .model3.json file." : expression
            ? "Please choose an .exp3.json file." : "Please choose a .motion3.json file.");
    path = std::move(chosen);
    return true;
}

bool matchesQuery(const std::wstring& value, const std::wstring& query) {
    if (query.empty()) return true;
    if (query.size() > value.size()) return false;
    for (size_t i = 0; i <= value.size() - query.size(); ++i)
        if (CompareStringOrdinal(value.data() + i, static_cast<int>(query.size()),
            query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL) return true;
    return false;
}

std::wstring controlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length + 1), L'\0');
    result.resize(static_cast<size_t>(GetWindowTextW(control, result.data(), length + 1)));
    return result;
}

struct PickerList {
    std::vector<MotionEntry> entries;
    std::vector<size_t> visible;
    std::wstring selected;
    std::wstring query;
    int listId = 0;
    int searchId = 0;
    int pathId = 0;
    bool expression = false;
};

// There is exactly one queued request and one completed image. Rapid scrubbing
// replaces queued work, and stale selections can never appear in the preview.
class PreviewWorker {
public:
    struct Result {
        std::uint64_t generation = 0;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> bgra;
        double duration = 3.0;
        std::wstring error;
    };
    struct Library {
        std::vector<MotionEntry> motions;
        std::vector<MotionEntry> expressions;
        std::wstring error;
    };
    ~PreviewWorker() { stop(); }
    void start(const std::wstring& model) {
        worker_ = std::thread([this, model] { run(model); });
    }
    void stop() noexcept {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; hasRequest_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void submit(RenderRequest request, std::uint64_t generation, bool upperBody = true) {
        { std::lock_guard<std::mutex> lock(mutex_);
          if (stopping_) return;
          request_ = std::move(request); generation_ = generation; upperBody_ = upperBody; hasRequest_ = true; }
        cv_.notify_one();
    }
    bool takeLibrary(Library& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasLibrary_) return false;
        result = std::move(library_); hasLibrary_ = false; return true;
    }
    bool takeResult(Result& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasResult_) return false;
        result = std::move(result_); hasResult_ = false; return true;
    }
private:
    void run(const std::wstring& model) noexcept {
        try {
            Library library;
            try { library.motions = listMotions(model); library.expressions = listExpressions(model); }
            catch (const std::exception& e) { library.error = fromUtf8(e.what()); }
            catch (...) { library.error = uiText(L"The model file could not be read.", L"模型文件无法读取。"); }
            { std::lock_guard<std::mutex> lock(mutex_);
              if (stopping_) return;
              library_ = std::move(library); hasLibrary_ = true; }
            std::wstring durationPath;
            double cachedDuration = 3.0;
            bool durationReady = false;
            PreviewBounds framing;
            bool calibrated = false;
            int calibrationAttempts = 0;
            for (;;) {
                RenderRequest request;
                std::uint64_t generation = 0;
                bool upperBody = true;
                { std::unique_lock<std::mutex> lock(mutex_);
                  cv_.wait(lock, [this] { return stopping_ || hasRequest_; });
                  if (stopping_) return;
                  request = std::move(request_); generation = generation_; upperBody = upperBody_; hasRequest_ = false; }
                Result result;
                result.generation = generation;
                try {
                    if (!durationReady || request.motionPath != durationPath) {
                        const auto duration = request.motionPath.empty() ? 3.0 : motionDuration(request.motionPath);
                        if (!std::isfinite(duration) || duration <= 0)
                            throw std::runtime_error("The selected motion has no valid duration.");
                        durationPath = request.motionPath;
                        cachedDuration = duration;
                        durationReady = true;
                    }
                    result.duration = cachedDuration;
                    if (!calibrated && calibrationAttempts < 3 && request.width > 0 && request.height > 0) {
                        ++calibrationAttempts;
                        auto neutral = request;
                        neutral.motionPath.clear(); neutral.motionPathB.clear(); neutral.blend = 0;
                        neutral.expressionPathA.clear(); neutral.expressionPathB.clear();
                        neutral.expressionWeightA = neutral.expressionWeightB = 0;
                        neutral.breathingEnabled = neutral.autoBlinkEnabled = neutral.lipSyncEnabled = false;
                        neutral.seconds = neutral.secondsB = neutral.ambientSeconds = 0;
                        neutral.scale = 1; neutral.offsetX = neutral.offsetY = 0;
                        framing = alphaBounds(render(neutral));
                        calibrated = framing.valid;
                        { std::lock_guard<std::mutex> lock(mutex_);
                          if (stopping_) return;
                          if (generation != generation_) continue; }
                    }
                    auto rendered = render(framePreview(std::move(request), framing, upperBody));
                    if (rendered.width <= 0 || rendered.height <= 0 ||
                        rendered.rgba.size() != static_cast<size_t>(rendered.width) * rendered.height * 4)
                        throw std::runtime_error("The renderer returned an invalid preview image.");
                    result.width = rendered.width;
                    result.height = rendered.height;
                    result.bgra.resize(rendered.rgba.size());
                    for (int y = 0; y < rendered.height; ++y) for (int x = 0; x < rendered.width; ++x) {
                        const size_t offset = (static_cast<size_t>(y) * rendered.width + x) * 4;
                        const int checker = ((x / 16 + y / 16) & 1) ? 35 : 39;
                        const int background = ((255 - rendered.rgba[offset + 3]) * checker + 127) / 255;
                        result.bgra[offset] = static_cast<std::uint8_t>(std::min(255, rendered.rgba[offset + 2] + background));
                        result.bgra[offset + 1] = static_cast<std::uint8_t>(std::min(255, rendered.rgba[offset + 1] + background));
                        result.bgra[offset + 2] = static_cast<std::uint8_t>(std::min(255, rendered.rgba[offset] + background));
                        result.bgra[offset + 3] = 255;
                    }
                } catch (const std::exception& e) { result.error = fromUtf8(e.what()); }
                  catch (...) { result.error = uiText(L"Preview is temporarily unavailable.", L"预览暂时不可用。"); }
                { std::lock_guard<std::mutex> lock(mutex_);
                  if (stopping_) return;
                  if (generation == generation_) { result_ = std::move(result); hasResult_ = true; } }
            }
        } catch (...) {
            // Nothing may escape a worker thread into AE. Allocation failures
            // leave the dialog cancellable without invoking host callbacks.
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopping_ = false;
    bool hasRequest_ = false;
    bool hasResult_ = false;
    bool hasLibrary_ = false;
    RenderRequest request_;
    std::uint64_t generation_ = 0;
    bool upperBody_ = true;
    Result result_;
    Library library_;
};

struct DialogState {
    struct ControlLayout { HWND window; int x, y, width, height; };
    ModelSelection pending;
    AnimationImportSelection result;
    PickerList motions{{}, {}, {}, {}, kMotionList, kSearchMotion, kMotionPath, false};
    PickerList expressions{{}, {}, {}, {}, kExpressionList, kSearchExpression, kExpressionPath, true};
    RenderRequest previewSettings;
    PreviewWorker worker;
    PreviewWorker::Result frame;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT boldFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH cardBrush = nullptr;
    HBRUSH inputBrush = nullptr;
    std::vector<ControlLayout> controlLayouts;
    int dpi = 96;
    int transitionFrames = 30;
    int transitionCurve = 5;
    bool controlsReady = false;
    bool rebuilding = false;
    bool modelValid = false;
    bool playing = true;
    bool upperBody = true;
    bool appendClip = true;
    int hoveredButton = 0;
    bool previewDirty = true;
    bool previewEnabled = true; // Hidden smoke tests can exercise controls alone.
    std::uint64_t generation = 1;
    double seconds = 0;
    double duration = 3;
    ULONGLONG lastTick = 0;
    ~DialogState() {
        worker.stop();
        for (const auto handle : {font, titleFont, smallFont, boldFont}) if (handle) DeleteObject(handle);
        for (const auto handle : {backgroundBrush, cardBrush, inputBrush}) if (handle) DeleteObject(handle);
    }
};

void applyImportPreferences(DialogState& state, const AnimationImportSelection& preferences) {
    state.transitionFrames = preferences.transitionFrames >= 0 ? preferences.transitionFrames : 30;
    state.transitionCurve = preferences.transitionCurve >= 3 && preferences.transitionCurve <= 5
        ? preferences.transitionCurve : 5;
}

void addKnownPaths(PickerList& list, const std::vector<std::wstring>& bank) {
    const auto add = [&list](const std::wstring& path) {
        if (!path.empty() && std::none_of(list.entries.begin(), list.entries.end(),
            [&path](const MotionEntry& entry) { return entry.path == path; }))
            list.entries.push_back({animationName(path), path});
    };
    for (const auto& path : bank) add(path);
    add(list.selected);
}

void updateSummary(HWND window, const DialogState& state) {
    SetDlgItemTextW(window, kMotionPath, state.motions.selected.empty() ? uiText(L"No motion", L"未选择动作") : animationName(state.motions.selected).c_str());
    SetDlgItemTextW(window, kExpressionPath, state.expressions.selected.empty() ? uiText(L"No expression", L"未选择表情") : animationName(state.expressions.selected).c_str());
    const bool any = !state.motions.selected.empty() || !state.expressions.selected.empty();
    EnableWindow(GetDlgItem(window, IDOK), state.modelValid && any);
    for (const int id : {kBounceNone, kBounceGentle, kBounceStrong}) {
        const auto control = GetDlgItem(window, id);
        EnableWindow(control, state.modelValid && !state.motions.selected.empty());
        InvalidateRect(control, nullptr, FALSE);
    }
    const auto summary = !any ? uiText(L"Choose a motion or expression to preview and import.", L"选择动作或表情，即可预览并导入。") :
        !state.motions.selected.empty() && !state.expressions.selected.empty() ? uiText(L"Adds 1 motion clip and 1 expression clip", L"将添加 1 个动作片段和 1 个表情片段") :
        !state.motions.selected.empty() ? uiText(L"Adds 1 motion clip", L"将添加 1 个动作片段") : uiText(L"Adds 1 expression clip", L"将添加 1 个表情片段");
    SetDlgItemTextW(window, kImportSummary, summary);
}

void rebuildList(HWND window, DialogState& state, PickerList& list) {
    addKnownPaths(list, list.expression ? state.pending.expressionPaths : state.pending.motionPaths);
    HWND control = GetDlgItem(window, list.listId);
    struct Guard {
        HWND control; bool& flag;
        Guard(HWND c, bool& f) : control(c), flag(f) { flag = true; SendMessageW(c, WM_SETREDRAW, FALSE, 0); }
        ~Guard() { flag = false; SendMessageW(control, WM_SETREDRAW, TRUE, 0); InvalidateRect(control, nullptr, TRUE); }
    } guard(control, state.rebuilding);
    SendMessageW(control, LB_RESETCONTENT, 0, 0);
    list.visible.clear();
    const wchar_t* none = list.expression ? uiText(L"(Skip expression)", L"（不导入表情）") : uiText(L"(Skip motion)", L"（不导入动作）");
    if (SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(none)) != 0)
        throw std::runtime_error("Cannot populate the animation list.");
    int selected = list.selected.empty() ? 0 : -1;
    for (size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        if (!matchesQuery(entry.label, list.query) && !matchesQuery(entry.path, list.query)) continue;
        const auto name = animationName(entry.path);
        const auto row = SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (row == LB_ERR || row == LB_ERRSPACE) throw std::runtime_error("Cannot populate the animation list.");
        list.visible.push_back(i);
        if (entry.path == list.selected) selected = static_cast<int>(row);
    }
    SendMessageW(control, LB_SETCURSEL, selected, 0);
    const auto count = std::to_wstring(list.visible.size()) + (list.query.empty() ? std::wstring(uiText(L" items", L" 项")) : L" / " + std::to_wstring(list.entries.size()));
    SetDlgItemTextW(window, list.expression ? kExpressionCount : kMotionCount, count.c_str());
    updateSummary(window, state);
}

int ensureAsset(std::vector<std::wstring>& bank, const std::wstring& path) {
    const auto existing = std::find(bank.begin(), bank.end(), path);
    if (existing != bank.end()) return static_cast<int>(existing - bank.begin() + 1);
    const auto empty = std::find(bank.begin(), bank.end(), std::wstring{});
    if (empty != bank.end()) { *empty = path; return static_cast<int>(empty - bank.begin() + 1); }
    if (bank.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("This project has too many animation entries for After Effects.");
    bank.push_back(path);
    return static_cast<int>(bank.size());
}

AnimationImportSelection prepareImport(const DialogState& state, bool append, int transitionFrames) {
    if (state.motions.selected.empty() && state.expressions.selected.empty())
        throw std::runtime_error("Choose a motion, an expression, or both.");
    if (transitionFrames < 0 || transitionFrames > kMaximumTransitionFrames)
        throw std::runtime_error("Transition frames must be between 0 and 100000.");
    if (!state.motions.selected.empty() && (state.transitionCurve < 3 || state.transitionCurve > 5))
        throw std::runtime_error("Invalid motion transition curve.");
    AnimationImportSelection result;
    result.selection = state.pending;
    result.transitionFrames = transitionFrames;
    // The selection retains the preference even for expression-only imports;
    // the expression clip itself still uses its independent non-bounce fade.
    result.transitionCurve = state.transitionCurve;
    if (!state.motions.selected.empty()) {
        result.hasMotion = true;
        result.motion.duration = motionDuration(state.motions.selected);
        if (!std::isfinite(result.motion.duration) || result.motion.duration <= 0)
            throw std::runtime_error("The selected motion has no valid duration.");
        result.motion.slot = ensureAsset(result.selection.motionPaths, state.motions.selected);
        result.motion.label = animationName(state.motions.selected);
        result.motion.append = append;
        result.motion.transitionFrames = transitionFrames;
        result.motion.transitionCurve = state.transitionCurve;
    }
    if (!state.expressions.selected.empty()) {
        validateExpression(state.expressions.selected);
        result.hasExpression = true;
        result.expression.slot = ensureAsset(result.selection.expressionPaths, state.expressions.selected);
        result.expression.label = animationName(state.expressions.selected);
        result.expression.duration = result.hasMotion ? result.motion.duration : 3.0;
        result.expression.append = false;
        result.expression.transitionFrames = transitionFrames;
        result.expression.transitionCurve = 0;
    }
    result.motion.selection = result.selection;
    result.expression.selection = result.selection;
    return result;
}

void selectionChanged(HWND window, DialogState& state) {
    ++state.generation;
    state.seconds = 0;
    state.lastTick = GetTickCount64();
    state.previewDirty = true;
    state.frame = {};
    InvalidateRect(GetDlgItem(window, kPreview), nullptr, FALSE);
    updateSummary(window, state);
    SetDlgItemTextW(window, kStatus, uiText(L"Updating preview...", L"正在更新预览…"));
}

void submitPreview(DialogState& state) {
    RenderRequest request = state.previewSettings;
    request.modelPath = state.pending.modelPath;
    request.motionPath = state.motions.selected;
    request.motionPathB.clear();
    request.blend = 0;
    request.seconds = state.seconds;
    request.loop = true;
    request.expressionPathA = state.expressions.selected;
    request.expressionWeightA = state.expressions.selected.empty() ? 0.0f : 1.0f;
    request.expressionPathB.clear();
    request.expressionWeightB = 0;
    request.ambientSeconds = state.previewSettings.ambientSeconds + state.seconds;
    request.scale = 1;
    request.offsetX = 0;
    request.offsetY = 0;
    request.width = 384;
    request.height = 448;
    request.pixelAspect = 1;
    state.worker.submit(std::move(request), state.generation, state.upperBody);
    state.previewDirty = false;
}

void tick(HWND window, DialogState& state) {
    PreviewWorker::Library library;
    if (state.worker.takeLibrary(library)) {
        if (library.error.empty()) {
            state.motions.entries = std::move(library.motions);
            state.expressions.entries = std::move(library.expressions);
            state.modelValid = true;
            rebuildList(window, state, state.motions);
            rebuildList(window, state, state.expressions);
        } else {
            state.playing = false;
            SetDlgItemTextW(window, kPlay, uiText(L"Play", L"播放"));
            SetDlgItemTextW(window, kStatus, library.error.c_str());
        }
    }
    PreviewWorker::Result frame;
    if (state.worker.takeResult(frame) && frame.generation == state.generation) {
        if (frame.error.empty()) {
            state.duration = frame.duration;
            state.frame = std::move(frame);
            InvalidateRect(GetDlgItem(window, kPreview), nullptr, FALSE);
            SetDlgItemTextW(window, kStatus, uiText(L"Looping preview. Drag the slider to any moment.", L"循环预览 · 拖动进度可查看任意时刻"));
        } else {
            state.playing = false;
            SetDlgItemTextW(window, kPlay, uiText(L"Play", L"播放"));
            SetDlgItemTextW(window, kStatus, (std::wstring(uiText(L"Preview failed: ", L"预览失败：")) + frame.error).c_str());
        }
    }
    const auto now = GetTickCount64();
    if (state.playing && state.modelValid && !state.frame.bgra.empty()) {
        state.seconds = std::fmod(state.seconds + static_cast<double>(now - state.lastTick) / 1000.0, state.duration);
        state.previewDirty = true;
    }
    state.lastTick = now;
    InvalidateRect(GetDlgItem(window, kScrub), nullptr, FALSE);
    wchar_t time[64]{};
    swprintf_s(time, simplifiedChineseUi() ? L"%.2f / %.2f 秒" : L"%.2f / %.2f s", state.seconds, state.duration);
    SetDlgItemTextW(window, kTime, time);
    if (state.previewEnabled && state.previewDirty) submitPreview(state);
}

HWND addControl(HWND parent, DialogState& state, const wchar_t* kind, const wchar_t* text,
    DWORD style, DWORD extended, int x, int y, int width, int height, int id) {
    const auto px = [&state](int v) { return MulDiv(v, state.dpi, 96); };
    const auto control = CreateWindowExW(extended, kind, text, WS_CHILD | WS_VISIBLE | style,
        px(x), px(y), px(width), px(height), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (!control) throw std::runtime_error("Cannot create Live2D import controls.");
    state.controlLayouts.push_back({control, x, y, width, height});
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

LRESULT CALLBACK buttonProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(window, L"Live2DOriginalButtonProc"));
    const auto parent = GetParent(window);
    auto* state = parent ? reinterpret_cast<DialogState*>(GetWindowLongPtrW(parent, DWLP_USER)) : nullptr;
    const int id = GetDlgCtrlID(window);
    if (id >= kBounceNone && id <= kBounceStrong && state) {
        if (message == WM_GETDLGCODE)
            return (original ? CallWindowProcW(original, window, message, wParam, lParam) : DLGC_BUTTON) | DLGC_WANTARROWS;
        if (message == WM_KEYDOWN && IsWindowEnabled(window) &&
            (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN)) {
            const int next = kBounceNone + (id - kBounceNone +
                (wParam == VK_LEFT || wParam == VK_UP ? 2 : 1)) % 3;
            const auto control = GetDlgItem(parent, next);
            SetFocus(control);
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(next, BN_CLICKED), reinterpret_cast<LPARAM>(control));
            return 0;
        }
    }
    if (GetDlgCtrlID(window) == kScrub && state) {
        if (message == WM_GETDLGCODE) return DLGC_WANTARROWS;
        if (message == WM_LBUTTONDOWN || (message == WM_MOUSEMOVE && GetCapture() == window)) {
            if (message == WM_LBUTTONDOWN) { SetFocus(window); SetCapture(window); }
            RECT rect{}; GetClientRect(window, &rect);
            const int inset = MulDiv(7, state->dpi, 96), x = static_cast<short>(LOWORD(lParam));
            const int position = std::clamp((x - inset) * 1000 / std::max(1, static_cast<int>(rect.right) - 2 * inset), 0, 1000);
            SendMessageW(parent, kSeekPreview, position, 0); return 0;
        }
        if (message == WM_LBUTTONUP && GetCapture() == window) { ReleaseCapture(); return 0; }
        if (message == WM_KEYDOWN && (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END)) {
            const int current = static_cast<int>(1000.0 * state->seconds / state->duration);
            const int position = wParam == VK_HOME ? 0 : wParam == VK_END ? 1000 : current + (wParam == VK_LEFT ? -10 : 10);
            SendMessageW(parent, kSeekPreview, std::clamp(position, 0, 1000), 0); return 0;
        }
    }
    if (message == BM_SETSTYLE && GetDlgCtrlID(window) != kScrub) wParam = (wParam & ~BS_TYPEMASK) | BS_OWNERDRAW;
    if (state && message == WM_MOUSEMOVE && state->hoveredButton != GetDlgCtrlID(window)) {
        if (state->hoveredButton) InvalidateRect(GetDlgItem(parent, state->hoveredButton), nullptr, FALSE);
        state->hoveredButton = GetDlgCtrlID(window);
        TRACKMOUSEEVENT event{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0}; TrackMouseEvent(&event);
        InvalidateRect(window, nullptr, FALSE);
    }
    if (state && message == WM_MOUSELEAVE && state->hoveredButton == GetDlgCtrlID(window)) {
        state->hoveredButton = 0; InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        InvalidateRect(window, nullptr, FALSE);
    const auto result = original ? CallWindowProcW(original, window, message, wParam, lParam)
                                : DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemovePropW(window, L"Live2DOriginalButtonProc");
    return result;
}

void rounded(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, border);
    const auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}

int windowDpi(HWND window) {
    using GetDpi = UINT(WINAPI*)(HWND);
    const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi) { const auto dpi = getDpi(window); if (dpi) return static_cast<int>(dpi); }
    const auto dc = GetDC(window); const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(window, dc); return dpi;
}

void windowBounds(HWND window, RECT& bounds, int dpi) {
    using Adjust = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjust = reinterpret_cast<Adjust>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if (adjust) adjust(&bounds, style, FALSE, extended, dpi);
    else AdjustWindowRectEx(&bounds, style, FALSE, extended);
}

void refreshFonts(DialogState& state) {
    const HFONT old[]{state.font, state.titleFont, state.smallFont, state.boldFont};
    const auto font = [&state](int size, int weight) { return CreateFontW(-MulDiv(size, state.dpi, 72), 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI"); };
    state.font = font(10, FW_NORMAL); state.titleFont = font(19, FW_SEMIBOLD);
    state.smallFont = font(9, FW_NORMAL); state.boldFont = font(11, FW_SEMIBOLD);
    const HFONT replacement[]{state.font, state.titleFont, state.smallFont, state.boldFont};
    for (const auto& control : state.controlLayouts) {
        const auto current = reinterpret_cast<HFONT>(SendMessageW(control.window, WM_GETFONT, 0, 0));
        size_t kind = 0;
        for (size_t i = 0; i < 4; ++i) if (current == old[i]) { kind = i; break; }
        SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(replacement[kind]), TRUE);
    }
    for (const auto handle : old) if (handle) DeleteObject(handle);
}

void reflowDpi(HWND window, DialogState& state, int requestedDpi, const RECT& suggested) {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromRect(&suggested, MONITOR_DEFAULTTONEAREST), &monitor);
    state.dpi = fittedDpi(requestedDpi, monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top);
    refreshFonts(state);
    const auto px = [&state](int v) { return MulDiv(v, state.dpi, 96); };
    RECT bounds{0, 0, px(kLayoutWidth), px(kLayoutHeight)}; windowBounds(window, bounds, requestedDpi);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const int x = std::max(monitor.rcWork.left, std::min(suggested.left, monitor.rcWork.right - width));
    const int y = std::max(monitor.rcWork.top, std::min(suggested.top, monitor.rcWork.bottom - height));
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    for (const auto& control : state.controlLayouts)
        SetWindowPos(control.window, nullptr, px(control.x), px(control.y), px(control.width), px(control.height), SWP_NOZORDER | SWP_NOACTIVATE);
    for (const int list : {kMotionList, kExpressionList}) SendDlgItemMessageW(window, list, LB_SETITEMHEIGHT, 0, px(32));
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void initialize(HWND window, DialogState& state) {
    const auto owner = GetWindow(window, GW_OWNER);
    // The dialog owns all DPI layout, including owner-drawn cards and fonts.
    // PMv2's default automatic child reflow must not independently rescale it.
    using SetBehavior = BOOL(WINAPI*)(HWND, int, int);
    const auto setBehavior = reinterpret_cast<SetBehavior>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetDialogDpiChangeBehavior"));
    if (setBehavior) setBehavior(window, 1 /* DDC_DISABLE_ALL */, 1);
    const int actualDpi = windowDpi(owner ? owner : window);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(owner ? owner : window, MONITOR_DEFAULTTONEAREST), &monitor);
    state.dpi = fittedDpi(actualDpi, monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top);
    refreshFonts(state);
    state.backgroundBrush = CreateSolidBrush(kBackgroundColor);
    state.cardBrush = CreateSolidBrush(kCardColor); state.inputBrush = CreateSolidBrush(kInputColor);
    const auto px = [&state](int v) { return MulDiv(v, state.dpi, 96); };
    RECT bounds{0, 0, px(kLayoutWidth), px(kLayoutHeight)};
    windowBounds(window, bounds, actualDpi);
    RECT ownerRect = monitor.rcWork;
    if (owner) GetWindowRect(owner, &ownerRect);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const int left = std::max(monitor.rcWork.left, std::min(monitor.rcWork.right - width,
        ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2));
    const int top = std::max(monitor.rcWork.top, std::min(monitor.rcWork.bottom - height,
        ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2));
    SetWindowPos(window, nullptr, left, top, width, height, SWP_NOZORDER);
    // Optional Windows 10/11 dark title bar; no new link or runtime dependency.
    if (const auto dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        const auto setAttribute = reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (setAttribute) { const BOOL dark = TRUE; setAttribute(window, 20, &dark, sizeof(dark)); }
        FreeLibrary(dwm);
    }
    const auto label = [&](const wchar_t* value, int x, int y, int w, int h, int id = 0, bool small = false) {
        auto c = addControl(window, state, L"STATIC", value, SS_ENDELLIPSIS, 0, x, y, w, h, id);
        if (small) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(state.smallFont), TRUE);
        return c;
    };
    const auto button = [&](const wchar_t* value, int x, int y, int w, int h, int id) {
        auto c = addControl(window, state, L"BUTTON", value, BS_OWNERDRAW | WS_TABSTOP, 0, x, y, w, h, id);
        const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(c, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(buttonProcedure)));
        SetPropW(c, L"Live2DOriginalButtonProc", reinterpret_cast<HANDLE>(original)); return c;
    };
    auto heading = label(uiText(L"Import Motions & Expressions", L"导入动作与表情"), 24, 22, 600, 38, kHeading);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(state.titleFont), TRUE);
    label(uiText(L"Choose a motion and expression, preview them, then add both to the timeline.", L"选好动作与表情，预览后一起添加到时间线。"), 26, 67, 720, 24, kSubtitle);
    auto modelName = animationName(state.pending.modelPath);
    if (hasSuffix(modelName, L".model3.json")) modelName.resize(modelName.size() - 12);
    label((std::wstring(uiText(L"Current model  ·  ", L"当前模型  ·  ")) + modelName).c_str(), 708, 37, 308, 28, kModelPath);
    for (const bool expression : {false, true}) {
        auto& list = expression ? state.expressions : state.motions;
        const int x = expression ? 294 : 24;
        auto title = label(expression ? uiText(L"Expression", L"表情") : uiText(L"Motion", L"动作"), x + 16, 122, 124, 28);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
        label(uiText(L"Loading", L"加载中"), x + 152, 126, 90, 22, expression ? kExpressionCount : kMotionCount, true);
        label(uiText(L"Search", L"搜索"), x + 16, 168, 62, 22, expression ? kExpressionSearchHint : kMotionSearchHint, true);
        auto search = addControl(window, state, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            0, x + 80, 166, 160, 28, list.searchId);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(expression ? uiText(L"Search expressions...", L"搜索表情…") : uiText(L"Search motions...", L"搜索动作…")));
        SendMessageW(search, EM_SETLIMITTEXT, 1024, 0);
        const auto listControl = addControl(window, state, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, x + 12, 204, 234, 158, list.listId);
        SendMessageW(listControl, LB_SETITEMHEIGHT, 0, px(32));
        button(expression ? uiText(L"+ Expression File", L"＋ 外部表情文件") : uiText(L"+ Motion File", L"＋ 外部动作文件"), x + 16, 372, 226, 30,
            expression ? kBrowseExpression : kBrowseMotion);
        label(uiText(L"Selected", L"已选"), x + 16, 413, 70, 22, 0, true);
        label(expression ? uiText(L"No expression", L"未选择表情") : uiText(L"No motion", L"未选择动作"), x + 88, 411, 156, 24, list.pathId);
    }
    auto previewTitle = label(uiText(L"Live Preview", L"实时预览"), 592, 124, 190, 26);
    SendMessageW(previewTitle, WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
    button(uiText(L"Half", L"半身"), 834, 118, 78, 32, kUpperBody);
    button(uiText(L"Full", L"全身"), 918, 118, 78, 32, kFullBody);
    addControl(window, state, L"STATIC", L"", SS_OWNERDRAW, 0, 592, 158, 406, 400, kPreview);
    button(uiText(L"Pause", L"暂停"), 592, 572, 76, 30, kPlay);
    const auto scrub = addControl(window, state, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY | WS_TABSTOP, 0, 676, 575, 200, 26, kScrub);
    const auto originalScrub = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(scrub, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(buttonProcedure)));
    SetPropW(scrub, L"Live2DOriginalButtonProc", reinterpret_cast<HANDLE>(originalScrub));
    label(uiText(L"0.00 / 3.00 s", L"0.00 / 3.00 秒"), 888, 576, 114, 24, kTime, true);
    label(uiText(L"Loading preview. The first load can take a few seconds...", L"正在加载预览，首次加载需要数秒…"), 576, 625, 440, 22, kStatus, true);
    label(uiText(L"Transition", L"过渡时长"), 40, 470, 166, 20, kTransitionTitle);
    // The edit sits centered inside a 32px visual field, matching the buttons.
    // A single-line native edit has no vertical-alignment option of its own.
    auto edit = addControl(window, state, L"EDIT", std::to_wstring(state.transitionFrames).c_str(),
        ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP, 0, 46, 502, 78, 20, kTransition);
    SendMessageW(edit, EM_SETLIMITTEXT, 6, 0);
    addControl(window, state, L"STATIC", uiText(L"frames", L"帧"), SS_CENTERIMAGE, 0, 128, 496, 78, 32, kTransitionUnit);
    label(uiText(L"Overshoot", L"回弹幅度"), 250, 470, 286, 20, kBounceTitle);
    button(uiText(L"None", L"无"), 250, 496, 90, 32, kBounceNone);
    button(uiText(L"Soft", L"轻柔"), 348, 496, 90, 32, kBounceGentle);
    button(uiText(L"Strong", L"明显"), 446, 496, 90, 32, kBounceStrong);
    label(uiText(L"Placement", L"添加位置"), 40, 560, 496, 20, kPlacementTitle);
    button(uiText(L"After Previous Clip", L"接在上一动作后"), 40, 586, 242, 32, kAppendClip);
    button(uiText(L"At Current Time", L"从当前时间开始"), 294, 586, 242, 32, kAtPlayhead);
    label(uiText(L"Choose a motion or expression to preview and import.", L"选择动作或表情，即可预览并导入。"), 26, 663, 640, 24, kImportSummary, true);
    button(uiText(L"Cancel", L"取消"), 706, 651, 94, 36, IDCANCEL);
    button(uiText(L"Add to Timeline", L"添加到时间线"), 814, 651, 202, 36, IDOK);
    SendMessageW(window, DM_SETDEFID, IDOK, 0);
    state.controlsReady = true;
    rebuildList(window, state, state.motions); rebuildList(window, state, state.expressions);
    state.lastTick = GetTickCount64();
    if (state.previewEnabled) {
        state.worker.start(state.pending.modelPath); submitPreview(state);
        if (!SetTimer(window, kPreviewTimer, 42, nullptr)) throw std::runtime_error("Cannot start the preview timer.");
    }
    SetFocus(GetDlgItem(window, kSearchMotion));
}

void paintDialog(HWND window, HDC dc, const DialogState& state) {
    const int saved = SaveDC(dc);
    // Explicit clipping also covers hosts that customize dialog styles.
    for (const auto& control : state.controlLayouts) {
        if (!IsWindowVisible(control.window)) continue;
        RECT rect{}; GetWindowRect(control.window, &rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        ExcludeClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    }
    RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, state.backgroundBrush);
    const auto px = [&state](int v) { return MulDiv(v, state.dpi, 96); };
    for (const auto& rect : {RECT{24, 106, 282, 446}, RECT{294, 106, 552, 446}, RECT{574, 106, 1016, 618},
            RECT{24, 458, 222, 538}, RECT{234, 458, 552, 538}, RECT{24, 550, 552, 628}})
        rounded(dc, {px(rect.left), px(rect.top), px(rect.right), px(rect.bottom)}, kCardColor, kBorderColor, px(14));
    for (const int x : {24, 294})
        rounded(dc, {px(x + 12), px(160), px(x + 246), px(198)}, kInputColor, kBorderColor, px(9));
    rounded(dc, {px(40), px(496), px(162), px(528)}, kInputColor, kBorderColor, px(8));
    const auto pen = CreatePen(PS_SOLID, 1, kBorderColor);
    const auto previous = SelectObject(dc, pen);
    MoveToEx(dc, px(24), px(641), nullptr); LineTo(dc, px(1016), px(641)); SelectObject(dc, previous); DeleteObject(pen);
    RestoreDC(dc, saved);
}

bool buttonChosen(int id, const DialogState& state) {
    return (id == kUpperBody && state.upperBody) || (id == kFullBody && !state.upperBody) ||
        (id == kAppendClip && state.appendClip) || (id == kAtPlayhead && !state.appendClip) ||
        (id >= kBounceNone && id <= kBounceStrong && state.transitionCurve == 3 + id - kBounceNone);
}
void drawButton(const DRAWITEMSTRUCT& item, const DialogState& state) {
    const bool enabled = !(item.itemState & ODS_DISABLED), primary = item.CtlID == IDOK;
    const bool chosen = buttonChosen(static_cast<int>(item.CtlID), state);
    const bool hover = state.hoveredButton == static_cast<int>(item.CtlID), down = item.itemState & ODS_SELECTED;
    const auto background = primary ? enabled ? (down ? RGB(117, 100, 224) : hover ? RGB(162, 148, 255) : kAccentColor) : RGB(63, 59, 85)
        : !enabled ? kInputColor : chosen ? RGB(58, 51, 88) : hover ? RGB(54, 60, 73) : kInputColor;
    FillRect(item.hDC, &item.rcItem, (item.CtlID == IDOK || item.CtlID == IDCANCEL) ? state.backgroundBrush : state.cardBrush);
    rounded(item.hDC, item.rcItem, background, chosen && enabled ? kAccentColor : primary ? background : kBorderColor, MulDiv(9, state.dpi, 96));
    auto rect = item.rcItem;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, !enabled ? RGB(128, 126, 148) : primary ? RGB(20, 18, 33) : kTextColor);
    const auto old = SelectObject(item.hDC, primary ? state.boldFont : state.font);
    const auto text = controlText(item.hwndItem);
    DrawTextW(item.hDC, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, old);
    if (item.itemState & ODS_FOCUS) {
        InflateRect(&rect, -4, -4); SetTextColor(item.hDC, kTextColor); SetBkColor(item.hDC, background); DrawFocusRect(item.hDC, &rect);
    }
}

void drawListItem(const DRAWITEMSTRUCT& item, const DialogState& state) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const bool selected = item.itemState & ODS_SELECTED;
    const auto brush = CreateSolidBrush(selected ? RGB(56, 49, 83) : kCardColor);
    FillRect(item.hDC, &item.rcItem, brush); DeleteObject(brush);
    auto textRect = item.rcItem;
    textRect.left += MulDiv(31, state.dpi, 96); textRect.right -= MulDiv(8, state.dpi, 96);
    const int count = static_cast<int>(SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0));
    if (count < 0) return;
    std::wstring text(static_cast<size_t>(count + 1), L'\0');
    SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data()));
    SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, item.itemID ? kTextColor : kMutedColor);
    const auto old = SelectObject(item.hDC, state.font);
    DrawTextW(item.hDC, text.c_str(), -1, &textRect, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, old);
    const int radius = MulDiv(5, state.dpi, 96), cx = item.rcItem.left + MulDiv(15, state.dpi, 96), cy = (item.rcItem.top + item.rcItem.bottom) / 2;
    const auto pen = CreatePen(PS_SOLID, 1, selected ? kAccentColor : kBorderColor);
    const auto dot = CreateSolidBrush(selected ? kAccentColor : kCardColor);
    const auto oldPen = SelectObject(item.hDC, pen), oldBrush = SelectObject(item.hDC, dot);
    Ellipse(item.hDC, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(item.hDC, oldPen); SelectObject(item.hDC, oldBrush); DeleteObject(pen); DeleteObject(dot);
    if (item.itemState & ODS_FOCUS) { auto rect = item.rcItem; InflateRect(&rect, -2, -2); DrawFocusRect(item.hDC, &rect); }
}

void drawPreviewImage(const DRAWITEMSTRUCT& item, const DialogState& state) {
    FillRect(item.hDC, &item.rcItem, state.cardBrush);
    if (state.frame.bgra.empty()) {
        SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, kMutedColor);
        auto rect = item.rcItem; const auto old = SelectObject(item.hDC, state.font);
        DrawTextW(item.hDC, uiText(L"Preparing the character preview...", L"正在准备角色预览…"), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old); return;
    }
    const float scale = std::min(static_cast<float>(item.rcItem.right - item.rcItem.left) / state.frame.width,
        static_cast<float>(item.rcItem.bottom - item.rcItem.top) / state.frame.height);
    const int width = static_cast<int>(state.frame.width * scale), height = static_cast<int>(state.frame.height * scale);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = state.frame.width;
    info.bmiHeader.biHeight = -state.frame.height; info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(item.hDC, HALFTONE); SetBrushOrgEx(item.hDC, 0, 0, nullptr);
    StretchDIBits(item.hDC, item.rcItem.left + (item.rcItem.right - item.rcItem.left - width) / 2,
        item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - height) / 2, width, height,
        0, 0, state.frame.width, state.frame.height, state.frame.bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

void drawPreview(const DRAWITEMSTRUCT& item, const DialogState& state) {
    const int width = item.rcItem.right - item.rcItem.left, height = item.rcItem.bottom - item.rcItem.top;
    const auto buffer = CreateCompatibleDC(item.hDC);
    const auto bitmap = CreateCompatibleBitmap(item.hDC, width, height);
    if (!buffer || !bitmap) {
        if (buffer) DeleteDC(buffer); if (bitmap) DeleteObject(bitmap);
        drawPreviewImage(item, state); return;
    }
    const auto old = SelectObject(buffer, bitmap);
    auto pending = item; pending.hDC = buffer; pending.rcItem = {0, 0, width, height};
    drawPreviewImage(pending, state);
    BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer);
}

void drawScrub(const DRAWITEMSTRUCT& item, const DialogState& state) {
    FillRect(item.hDC, &item.rcItem, state.cardBrush);
    const int inset = MulDiv(7, state.dpi, 96), half = MulDiv(2, state.dpi, 96);
    const int center = (item.rcItem.top + item.rcItem.bottom) / 2;
    const int left = item.rcItem.left + inset, right = item.rcItem.right - inset;
    const int progress = left + static_cast<int>((right - left) * std::clamp(state.seconds / state.duration, 0.0, 1.0));
    rounded(item.hDC, {left, center - half, right, center + half}, kBorderColor, kBorderColor, half * 2);
    if (progress > left) rounded(item.hDC, {left, center - half, progress, center + half}, kAccentColor, kAccentColor, half * 2);
    rounded(item.hDC, {progress - inset, center - inset, progress + inset, center + inset}, kTextColor, kTextColor, inset * 2);
    if (GetFocus() == item.hwndItem) { auto rect = item.rcItem; InflateRect(&rect, -1, -1); DrawFocusRect(item.hDC, &rect); }
}

INT_PTR CALLBACK dialogProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<DialogState*>(lParam);
            SetWindowLongPtrW(window, DWLP_USER, lParam);
            initialize(window, *state);
            return FALSE;
        }
        if (!state) return FALSE;
        if (message == WM_DPICHANGED) {
            if (state->controlsReady) reflowDpi(window, *state, LOWORD(wParam), *reinterpret_cast<const RECT*>(lParam));
            return TRUE;
        }
        if (message == WM_ERASEBKGND) return TRUE; // WM_PAINT draws the complete background without an intermediate erase.
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint); paintDialog(window, dc, *state); EndPaint(window, &paint); return TRUE;
        }
        if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(state->backgroundBrush);
        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC) {
            const auto dc = reinterpret_cast<HDC>(wParam);
            const auto control = reinterpret_cast<HWND>(lParam);
            const int id = GetDlgCtrlID(control);
            const bool input = id == kSearchMotion || id == kSearchExpression || id == kTransition ||
                id == kMotionSearchHint || id == kExpressionSearchHint;
            RECT rect{}; GetWindowRect(control, &rect); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
            const int y = MulDiv(rect.top, 96, state->dpi);
            const bool background = y < 106 || y >= 618;
            const bool muted = id == kStatus || id == kSubtitle || id == kModelPath || id == kTime || id == kMotionCount ||
                id == kExpressionCount || id == kImportSummary || id == kMotionSearchHint || id == kExpressionSearchHint;
            SetTextColor(dc, muted ? kMutedColor : kTextColor);
            SetBkColor(dc, input ? kInputColor : background ? kBackgroundColor : kCardColor);
            return reinterpret_cast<INT_PTR>(input ? state->inputBrush : background ? state->backgroundBrush : state->cardBrush);
        }
        if (message == WM_MEASUREITEM) {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlType == ODT_LISTBOX) { measure->itemHeight = MulDiv(32, state->dpi, 96); return TRUE; }
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message == WM_DESTROY) { KillTimer(window, kPreviewTimer); return TRUE; }
        if (message == WM_TIMER && wParam == kPreviewTimer) { tick(window, *state); return TRUE; }
        if (message == WM_DRAWITEM) {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (wParam == kPreview) drawPreview(item, *state);
            else if (wParam == kScrub) drawScrub(item, *state);
            else if (item.CtlType == ODT_BUTTON) drawButton(item, *state);
            else if (item.CtlType == ODT_LISTBOX) drawListItem(item, *state);
            else return FALSE;
            return TRUE;
        }
        if (message == kSeekPreview) {
            state->playing = false;
            state->seconds = std::clamp(static_cast<int>(wParam), 0, 1000) * state->duration / 1000.0;
            state->previewDirty = true;
            SetDlgItemTextW(window, kPlay, uiText(L"Play", L"播放"));
            tick(window, *state); return TRUE;
        }
        if (message == WM_HSCROLL && reinterpret_cast<HWND>(lParam) == GetDlgItem(window, kScrub)) {
            SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL};
            GetScrollInfo(reinterpret_cast<HWND>(lParam), SB_CTL, &info);
            int position = info.nPos;
            switch (LOWORD(wParam)) {
            case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
            case SB_LINELEFT: position -= 10; break;
            case SB_LINERIGHT: position += 10; break;
            case SB_PAGELEFT: position -= 100; break;
            case SB_PAGERIGHT: position += 100; break;
            case SB_LEFT: position = 0; break;
            case SB_RIGHT: position = 1000; break;
            default: return TRUE;
            }
            state->playing = false;
            state->seconds = std::max(0, std::min(1000, position)) * state->duration / 1000.0;
            state->previewDirty = true;
            SetDlgItemTextW(window, kPlay, uiText(L"Play", L"播放"));
            tick(window, *state);
            return TRUE;
        }
        if (message == WM_COMMAND) {
            const int id = LOWORD(wParam), notification = HIWORD(wParam);
            if (id >= kBounceNone && id <= kBounceStrong) {
                if (notification != BN_CLICKED || !state->modelValid || state->motions.selected.empty() ||
                    !IsWindowEnabled(GetDlgItem(window, id))) return TRUE;
                state->transitionCurve = 3 + id - kBounceNone;
                for (const int control : {kBounceNone, kBounceGentle, kBounceStrong})
                    InvalidateRect(GetDlgItem(window, control), nullptr, FALSE);
                return TRUE;
            }
            if (id == kUpperBody || id == kFullBody) {
                state->upperBody = id == kUpperBody;
                ++state->generation; state->previewDirty = true; state->frame = {};
                InvalidateRect(GetDlgItem(window, kUpperBody), nullptr, FALSE);
                InvalidateRect(GetDlgItem(window, kFullBody), nullptr, FALSE);
                InvalidateRect(GetDlgItem(window, kPreview), nullptr, FALSE);
                return TRUE;
            }
            if (id == kAppendClip || id == kAtPlayhead) {
                state->appendClip = id == kAppendClip;
                InvalidateRect(GetDlgItem(window, kAppendClip), nullptr, FALSE);
                InvalidateRect(GetDlgItem(window, kAtPlayhead), nullptr, FALSE);
                return TRUE;
            }
            for (auto* list : {&state->motions, &state->expressions}) {
                if (id == list->searchId && notification == EN_CHANGE && state->controlsReady) {
                    list->query = controlText(GetDlgItem(window, id));
                    rebuildList(window, *state, *list);
                    return TRUE;
                }
                if (id == list->listId && !state->rebuilding && (notification == LBN_SELCHANGE || notification == LBN_DBLCLK)) {
                    const auto row = SendDlgItemMessageW(window, id, LB_GETCURSEL, 0, 0);
                    if (row == 0) list->selected.clear();
                    else if (row > 0 && static_cast<size_t>(row) <= list->visible.size())
                        list->selected = list->entries[list->visible[static_cast<size_t>(row - 1)]].path;
                    else return TRUE; // Filtering out the current selection never clears it.
                    selectionChanged(window, *state);
                    return TRUE;
                }
            }
            if (id == kBrowseMotion || id == kBrowseExpression) {
                auto& list = id == kBrowseExpression ? state->expressions : state->motions;
                auto path = list.selected;
                if (browseFile(window, false, path, list.expression)) {
                    list.selected = std::move(path);
                    rebuildList(window, *state, list);
                    selectionChanged(window, *state);
                }
                return TRUE;
            }
            if (id == kPlay) {
                state->playing = !state->playing;
                state->lastTick = GetTickCount64();
                state->previewDirty = true;
                SetDlgItemTextW(window, kPlay, state->playing ? uiText(L"Pause", L"暂停") : uiText(L"Play", L"播放"));
                return TRUE;
            }
            if (id == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
            if (id == IDOK) {
                if (GetFocus() == GetDlgItem(window, kSearchMotion) || GetFocus() == GetDlgItem(window, kSearchExpression)) {
                    SetFocus(GetDlgItem(window, GetFocus() == GetDlgItem(window, kSearchExpression) ? kExpressionList : kMotionList));
                    return TRUE;
                }
                if (!state->modelValid) return TRUE;
                const auto value = controlText(GetDlgItem(window, kTransition));
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::runtime_error("Enter a non-negative whole number of transition frames.");
                const auto frames = std::stoll(value);
                if (frames > kMaximumTransitionFrames) throw std::runtime_error("Transition frames must be between 0 and 100000.");
                if (!std::filesystem::is_regular_file(state->pending.modelPath))
                    throw std::runtime_error("The model file is missing. Restore its original path before importing.");
                state->result = prepareImport(*state, state->appendClip, static_cast<int>(frames));
                EndDialog(window, IDOK);
                return TRUE;
            }
        }
    } catch (const std::exception& e) {
        MessageBoxW(window, fromUtf8(e.what()).c_str(), L"AeGO Flash", MB_OK | MB_ICONERROR);
        if (message == WM_INITDIALOG) EndDialog(window, IDCANCEL);
        return TRUE;
    } catch (...) {
        MessageBoxW(window, uiText(L"The window operation failed.", L"窗口操作失败。"), L"AeGO Flash", MB_OK | MB_ICONERROR);
        if (message == WM_INITDIALOG) EndDialog(window, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool runDialog(DialogState& state, void* ownerWindow) {
    const auto owner = checkedOwner(ownerWindow);
    struct alignas(DWORD) Template {
        DLGTEMPLATE dialog;
        WORD menu = 0;
        WORD windowClass = 0;
        wchar_t title[32]{};
    } data{};
    static_assert(offsetof(Template, menu) == 18, "DLGTEMPLATE must use Windows packing.");
    wcsncpy_s(data.title, uiText(L"AeGO Flash - Import Clips", L"AeGO Flash — 导入动作与表情"), _TRUNCATE);
    data.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_CLIPCHILDREN;
    data.dialog.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    data.dialog.cx = 600;
    data.dialog.cy = 360;
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &data.dialog,
        owner, dialogProcedure, reinterpret_cast<LPARAM>(&state));
    state.worker.stop(); // Join before DialogState or the plugin can be unloaded.
    if (result == -1) throw std::runtime_error("Windows could not create the Live2D import window.");
    return result == IDOK;
}
} // namespace

bool showModelImportDialog(ModelSelection& selection, void* ownerWindow) {
    auto path = selection.modelPath;
    if (!browseFile(checkedOwner(ownerWindow), true, path)) return false;
    // Validate metadata before committing; rendering and animation selection
    // are deliberately deferred to their own actions.
    (void)listMotions(path);
    (void)listExpressions(path);
    if (path != selection.modelPath) selection = ModelSelection{};
    selection.modelPath = std::move(path);
    return true;
}

bool showAnimationImportDialog(const ModelSelection& current, AnimationImportSelection& result,
    void* ownerWindow, const RenderRequest* previewSettings) {
    if (current.modelPath.empty()) throw std::runtime_error("Import a Live2D model first.");
    DialogState state;
    state.pending = current;
    applyImportPreferences(state, result);
    if (previewSettings) state.previewSettings = *previewSettings;
    if (!runDialog(state, ownerWindow)) return false;
    result = std::move(state.result);
    return true;
}

bool showModelMotionDialog(ModelSelection& selection, void* ownerWindow) {
    return showModelImportDialog(selection, ownerWindow);
}

bool showMotionClipDialog(const ModelSelection& current, MotionClipSelection& result, void* ownerWindow) {
    AnimationImportSelection combined;
    combined.transitionFrames = result.transitionFrames;
    combined.transitionCurve = result.transitionCurve;
    if (!showAnimationImportDialog(current, combined, ownerWindow) || !combined.hasMotion) return false;
    result = std::move(combined.motion);
    return true;
}

bool showExpressionClipDialog(const ModelSelection& current, MotionClipSelection& result, void* ownerWindow) {
    AnimationImportSelection combined;
    combined.transitionFrames = result.transitionFrames;
    combined.transitionCurve = result.transitionCurve;
    if (!showAnimationImportDialog(current, combined, ownerWindow) || !combined.hasExpression) return false;
    result = std::move(combined.expression);
    return true;
}
} // namespace l2dae
