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
constexpr int kMotionTitle = 138;
constexpr int kExpressionTitle = 139;
constexpr int kPreviewTitle = 140;
constexpr int kMotionSelectedCaption = 145;
constexpr int kExpressionSelectedCaption = 146;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT kSeekPreview = WM_APP + 29;
constexpr int kMaximumTransitionFrames = 100000;
constexpr int kLayoutWidth = 1040;
constexpr int kLayoutHeight = 700;
constexpr int kColumnTop = 80;
constexpr int kColumnBottom = 516;
constexpr int kSettingsTop = 528;
constexpr int kSettingsBottom = 600;
constexpr int kMotionLeft = 24;
constexpr int kMotionRight = 304;
constexpr int kExpressionLeft = 316;
constexpr int kExpressionRight = 596;
constexpr int kPreviewLeft = 608;
constexpr int kPreviewRight = 1016;
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
    bool animation = false;
    for (const auto* suffix : {L".motion3.json", L".exp3.json"}) {
        if (hasSuffix(name, suffix)) { name.resize(name.size() - wcslen(suffix)); animation = true; break; }
    }
    if (animation) for (const auto* prefix : {L"mtn_", L"exp_"}) {
        const size_t count = wcslen(prefix);
        if (name.size() >= count && _wcsnicmp(name.c_str(), prefix, static_cast<int>(count)) == 0) {
            name.erase(0, count); break;
        }
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

constexpr int kGridColumns = 3;

struct PickerList {
    std::vector<MotionEntry> entries;
    std::vector<size_t> visible;
    std::wstring selected;
    std::wstring query;
    int listId = 0;
    int searchId = 0;
    int pathId = 0;
    bool expression = false;
    int gridColumn = 0;
    int pressedCell = -1;
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
            int framedWidth = 0, framedHeight = 0;
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
                    if (request.width != framedWidth || request.height != framedHeight) {
                        calibrated = false;
                        calibrationAttempts = 0;
                        framedWidth = request.width;
                        framedHeight = request.height;
                    }
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
    RECT motionCard{};
    RECT expressionCard{};
    RECT previewCard{};
    RECT transitionCard{};
    RECT reboundCard{};
    RECT placementCard{};
    int transitionFrames = 30;
    int extraTransitionFrames = -1;
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
    const auto rebound = GetDlgItem(window, kBounceNone);
    EnableWindow(rebound, state.modelValid && !state.motions.selected.empty());
    InvalidateRect(rebound, nullptr, FALSE);
}

int pickerCells(const PickerList& list) {
    return 1 + static_cast<int>(list.visible.size());
}

int cellAtPoint(HWND list, int x, int y) {
    RECT bounds{}; GetClientRect(list, &bounds);
    const int width = std::max(1, static_cast<int>(bounds.right));
    const int itemHeight = std::max(1, static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
    const int top = std::max(0, static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
    const int row = top + std::max(0, y) / itemHeight;
    int column = 0;
    for (int candidate = 0; candidate < kGridColumns; ++candidate)
        if (x >= candidate * width / kGridColumns) column = candidate;
    return row * kGridColumns + column;
}

bool applyListCell(PickerList& list, int cell) {
    const int cells = pickerCells(list);
    if (cell < 0 || cell >= cells) return false;
    if (cell == 0) list.selected.clear();
    else list.selected = list.entries[list.visible[static_cast<size_t>(cell - 1)]].path;
    list.gridColumn = cell % kGridColumns;
    return true;
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
    list.pressedCell = -1;
    int selectedCell = list.selected.empty() ? 0 : -1;
    for (size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        if (!matchesQuery(entry.label, list.query) && !matchesQuery(entry.path, list.query)) continue;
        list.visible.push_back(i);
        if (entry.path == list.selected) selectedCell = static_cast<int>(list.visible.size());
    }
    const int rows = (pickerCells(list) + kGridColumns - 1) / kGridColumns;
    for (int row = 0; row < rows; ++row)
        if (SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"")) == LB_ERR)
            throw std::runtime_error("Cannot populate the animation list.");
    if (selectedCell >= 0) list.gridColumn = selectedCell % kGridColumns;
    SendMessageW(control, LB_SETCURSEL, selectedCell < 0 ? -1 : selectedCell / kGridColumns, 0);
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

void submitPreview(HWND window, DialogState& state) {
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
    RECT preview{};
    if (const auto control = GetDlgItem(window, kPreview)) GetClientRect(control, &preview);
    request.width = std::max(1, static_cast<int>(preview.right));
    request.height = std::max(1, static_cast<int>(preview.bottom));
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
    if (state.previewEnabled && state.previewDirty) submitPreview(window, state);
}

struct SettingChoice { const wchar_t* english; const wchar_t* chinese; int value; };
const SettingChoice kTransitionChoices[]{
    {L"0 frames", L"0 帧", 0}, {L"15 frames", L"15 帧", 15}, {L"30 frames", L"30 帧", 30},
    {L"45 frames", L"45 帧", 45}, {L"60 frames", L"60 帧", 60}};
const SettingChoice kReboundChoices[]{
    {L"None", L"无", 3}, {L"Soft", L"轻柔", 4}, {L"Strong", L"明显", 5}};
const SettingChoice kPlacementChoices[]{
    {L"After Previous Clip", L"接在上一动作后", 1}, {L"At Current Time", L"从当前时间开始", 0}};

void fillSettingCombo(HWND combo, DialogState& state, const SettingChoice* choices, int count, int selected) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selectedIndex = 0;
    bool found = false;
    for (int i = 0; i < count; ++i) {
        const auto label = uiText(choices[i].english, choices[i].chinese);
        if (SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)) < 0)
            throw std::runtime_error("Cannot populate a settings list.");
        if (choices[i].value == selected) { selectedIndex = i; found = true; }
    }
    if (!found) {
        wchar_t extra[64]{};
        swprintf_s(extra, simplifiedChineseUi() ? L"%d 帧" : L"%d frames", selected);
        selectedIndex = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(extra)));
        if (selectedIndex < 0) throw std::runtime_error("Cannot populate a settings list.");
        state.extraTransitionFrames = selected;
    }
    SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
}

int selectedSetting(HWND combo, const SettingChoice* choices, int count, int extra) {
    const auto index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index >= 0 && index < count) return choices[index].value;
    return extra;
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

HWND addCombo(HWND parent, DialogState& state, int x, int y, int width, int height, int id) {
    // The creation height is the closed field plus the dropped list. Windows keeps
    // that list height after the visible control is restored to one row.
    auto control = addControl(parent, state, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED | WS_VSCROLL | WS_TABSTOP,
        0, x, y, width, height + 180, id);
    state.controlLayouts.back().height = height;
    const auto px = [&state](int v) { return MulDiv(v, state.dpi, 96); };
    SetWindowPos(control, nullptr, px(x), px(y), px(width), px(height), SWP_NOZORDER | SWP_NOACTIVATE);
    if (const auto theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        if (const auto setTheme = reinterpret_cast<SetTheme>(GetProcAddress(theme, "SetWindowTheme")))
            setTheme(control, L"", L"");
        FreeLibrary(theme);
    }
    return control;
}

LRESULT CALLBACK buttonProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(window, L"Live2DOriginalButtonProc"));
    const auto parent = GetParent(window);
    auto* state = parent ? reinterpret_cast<DialogState*>(GetWindowLongPtrW(parent, DWLP_USER)) : nullptr;
    const int id = GetDlgCtrlID(window);
    if (id == kScrub && state) {
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

LRESULT CALLBACK listProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(window, L"Live2DOriginalListProc"));
    const auto parent = GetParent(window);
    auto* state = parent ? reinterpret_cast<DialogState*>(GetWindowLongPtrW(parent, DWLP_USER)) : nullptr;
    const int id = GetDlgCtrlID(window);
    auto* list = state && id == kExpressionList ? &state->expressions : state && id == kMotionList ? &state->motions : nullptr;
    const auto notify = [&](int note) {
        if (list && list->pressedCell >= 0)
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, note), reinterpret_cast<LPARAM>(window));
    };
    if (list && (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)) {
        list->pressedCell = cellAtPoint(window, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
        const auto result = original ? CallWindowProcW(original, window, message, wParam, lParam)
                                     : DefWindowProcW(window, message, wParam, lParam);
        notify(message == WM_LBUTTONDBLCLK ? LBN_DBLCLK : LBN_SELCHANGE);
        return result;
    }
    if (list && message == WM_KEYDOWN && (wParam == VK_LEFT || wParam == VK_RIGHT)) {
        int cell = list->selected.empty() ? 0 : -1;
        if (!list->selected.empty()) for (size_t i = 0; i < list->visible.size(); ++i)
            if (list->entries[list->visible[i]].path == list->selected) { cell = static_cast<int>(i) + 1; break; }
        cell += wParam == VK_RIGHT ? 1 : -1;
        if (cell < 0 || cell >= pickerCells(*list)) return 0;
        list->pressedCell = cell;
        SendMessageW(window, LB_SETCURSEL, cell / kGridColumns, 0);
        notify(LBN_SELCHANGE);
        return 0;
    }
    const auto result = original ? CallWindowProcW(original, window, message, wParam, lParam)
                                : DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemovePropW(window, L"Live2DOriginalListProc");
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

DialogState::ControlLayout* findLayout(DialogState& state, int id) {
    for (auto& control : state.controlLayouts)
        if (GetDlgCtrlID(control.window) == id) return &control;
    return nullptr;
}

void placeControl(DialogState& state, int id, int x, int y, int width, int height) {
    auto* layout = findLayout(state, id);
    if (!layout) return;
    layout->x = x; layout->y = y; layout->width = width; layout->height = height;
    const int dpi = std::max(1, state.dpi);
    SetWindowPos(layout->window, nullptr, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
        MulDiv(width, dpi, 96), MulDiv(height, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
}

void layoutDialog(HWND window, DialogState& state) {
    RECT client{}; GetClientRect(window, &client);
    const int dpi = std::max(1, state.dpi);
    const int width = MulDiv(client.right, 96, dpi);
    const int height = MulDiv(client.bottom, 96, dpi);
    if (width < 240 || height < 240) return;
    const int margin = 20, gap = 12, settingsHeight = 76, buttonHeight = 44;
    const int columnTop = 78;
    const int columnBottom = std::max(columnTop + 180, height - (12 + settingsHeight + 12 + 10 + buttonHeight + 16));
    const int inner = width - margin * 2;
    const int columns = std::max(0, inner - gap * 2);
    const int motionWidth = columns * 5 / 13;
    const int expressionWidth = columns * 5 / 13;
    const int previewWidth = columns - motionWidth - expressionWidth;
    const int motionLeft = margin;
    const int motionRight = motionLeft + motionWidth;
    const int expressionLeft = motionRight + gap;
    const int expressionRight = expressionLeft + expressionWidth;
    const int previewLeft = expressionRight + gap;
    const int previewRight = previewLeft + previewWidth;
    state.motionCard = {motionLeft, columnTop, motionRight, columnBottom};
    state.expressionCard = {expressionLeft, columnTop, expressionRight, columnBottom};
    state.previewCard = {previewLeft, columnTop, previewRight, columnBottom};
    const int settingsTop = columnBottom + 12;
    const int settingsBottom = settingsTop + settingsHeight;
    const int settingGap = gap;
    const int settingWidth = std::max(0, (inner - settingGap * 2) / 3);
    state.transitionCard = {margin, settingsTop, margin + settingWidth, settingsBottom};
    state.reboundCard = {margin + settingWidth + settingGap, settingsTop,
        margin + settingWidth * 2 + settingGap, settingsBottom};
    state.placementCard = {margin + (settingWidth + settingGap) * 2, settingsTop, width - margin, settingsBottom};

    const auto columnBody = [&](int left, int right, bool expression) {
        const int title = expression ? kExpressionTitle : kMotionTitle;
        const int count = expression ? kExpressionCount : kMotionCount;
        const int hint = expression ? kExpressionSearchHint : kMotionSearchHint;
        const int search = expression ? kSearchExpression : kSearchMotion;
        const int list = expression ? kExpressionList : kMotionList;
        const int caption = expression ? kExpressionSelectedCaption : kMotionSelectedCaption;
        const int path = expression ? kExpressionPath : kMotionPath;
        placeControl(state, title, left + 16, columnTop + 12, 140, 26);
        placeControl(state, count, right - 140, columnTop + 16, 124, 22);
        const int searchY = columnTop + 44;
        const int hintWidth = 58;
        const int fieldX = left + 16 + hintWidth + 14;
        placeControl(state, hint, left + 16, searchY, hintWidth, 28);
        placeControl(state, search, fieldX, searchY, std::max(40, right - 16 - fieldX), 28);
        const int listTop = columnTop + 80;
        const int listBottom = columnBottom - 48;
        placeControl(state, list, left + 12, listTop, right - left - 24, std::max(32, listBottom - listTop));
        placeControl(state, caption, left + 16, columnBottom - 36, 72, 22);
        placeControl(state, path, left + 92, columnBottom - 38, right - left - 108, 24);
    };
    columnBody(motionLeft, motionRight, false);
    columnBody(expressionLeft, expressionRight, true);
    placeControl(state, kPreviewTitle, previewLeft + 16, columnTop + 12, std::max(40, previewWidth - 180), 26);
    placeControl(state, kUpperBody, previewRight - 160, columnTop + 8, 70, 32);
    placeControl(state, kFullBody, previewRight - 84, columnTop + 8, 68, 32);
    const int previewTop = columnTop + 48;
    const int transport = columnBottom - 78;
    placeControl(state, kPreview, previewLeft + 16, previewTop, previewWidth - 32, std::max(48, transport - previewTop));
    placeControl(state, kPlay, previewLeft + 16, transport + 4, 76, 30);
    placeControl(state, kScrub, previewLeft + 100, transport + 6, std::max(40, previewWidth - 220), 26);
    placeControl(state, kTime, previewRight - 112, transport + 6, 96, 24);
    placeControl(state, kStatus, previewLeft + 16, transport + 40, previewWidth - 32, 28);
    const auto setting = [&](const RECT& card, int title, int combo) {
        placeControl(state, title, card.left + 16, card.top + 12, card.right - card.left - 32, 18);
        placeControl(state, combo, card.left + 16, card.top + 34, card.right - card.left - 32, 28);
    };
    setting(state.transitionCard, kTransitionTitle, kTransition);
    setting(state.reboundCard, kBounceTitle, kBounceNone);
    setting(state.placementCard, kPlacementTitle, kAppendClip);
    placeControl(state, kHeading, margin, 16, width - margin * 2, 34);
    placeControl(state, kSubtitle, margin, 50, width - margin * 2, 22);
    placeControl(state, IDOK, margin, height - 16 - buttonHeight, width - margin * 2, buttonHeight);
    for (const int list : {kMotionList, kExpressionList})
        SendDlgItemMessageW(window, list, LB_SETITEMHEIGHT, 0, MulDiv(32, dpi, 96));
}

void reflowDpi(HWND window, DialogState& state, int requestedDpi, const RECT& suggested) {
    state.dpi = std::max(60, requestedDpi);
    refreshFonts(state);
    if (suggested.right > suggested.left && suggested.bottom > suggested.top)
        SetWindowPos(window, nullptr, suggested.left, suggested.top,
            suggested.right - suggested.left, suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
    layoutDialog(window, state);
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
    state.dpi = std::max(96, actualDpi);
    refreshFonts(state);
    state.backgroundBrush = CreateSolidBrush(kBackgroundColor);
    state.cardBrush = CreateSolidBrush(kCardColor); state.inputBrush = CreateSolidBrush(kInputColor);
    const auto px = [&state](int v) { return MulDiv(v, state.dpi, 96); };
    const int workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    const int width = std::max(MulDiv(720, state.dpi, 96), workWidth * 3 / 5);
    const int height = std::max(MulDiv(560, state.dpi, 96), workHeight * 7 / 10);
    const int left = monitor.rcWork.left + (workWidth - std::min(width, workWidth)) / 2;
    const int top = monitor.rcWork.top + (workHeight - std::min(height, workHeight)) / 2;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowPos(window, nullptr, left, top, std::min(width, workWidth), std::min(height, workHeight), SWP_NOZORDER | SWP_FRAMECHANGED);
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
    auto heading = label(uiText(L"Import Motions & Expressions", L"导入动作与表情"), 0, 0, 10, 10, kHeading);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(state.titleFont), TRUE);
    label(uiText(L"Choose a motion and expression, preview them, then add both to the timeline.", L"选好动作与表情，预览后一起添加到时间线。"), 0, 0, 10, 10, kSubtitle);
    for (const bool expression : {false, true}) {
        auto& list = expression ? state.expressions : state.motions;
        auto title = label(expression ? uiText(L"Expression", L"表情") : uiText(L"Motion", L"动作"), 0, 0, 10, 10,
            expression ? kExpressionTitle : kMotionTitle);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
        label(uiText(L"Loading", L"加载中"), 0, 0, 10, 10, expression ? kExpressionCount : kMotionCount, true);
        addControl(window, state, L"STATIC", uiText(L"Search", L"搜索"), SS_OWNERDRAW, 0, 0, 0, 10, 10,
            expression ? kExpressionSearchHint : kMotionSearchHint);
        auto search = addControl(window, state, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 0, 10, 10, list.searchId);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(expression ? uiText(L"Search expressions...", L"搜索表情…") : uiText(L"Search motions...", L"搜索动作…")));
        SendMessageW(search, EM_SETLIMITTEXT, 1024, 0);
        const auto listControl = addControl(window, state, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 10, 10, list.listId);
        SendMessageW(listControl, LB_SETITEMHEIGHT, 0, px(32));
        const auto originalList = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(listControl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(listProcedure)));
        SetPropW(listControl, L"Live2DOriginalListProc", reinterpret_cast<HANDLE>(originalList));
        label(uiText(L"Selected", L"已选"), 0, 0, 10, 10, expression ? kExpressionSelectedCaption : kMotionSelectedCaption, true);
        label(expression ? uiText(L"No expression", L"未选择表情") : uiText(L"No motion", L"未选择动作"), 0, 0, 10, 10, list.pathId);
    }
    auto previewTitle = label(uiText(L"Live Preview", L"实时预览"), 0, 0, 10, 10, kPreviewTitle);
    SendMessageW(previewTitle, WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
    button(uiText(L"Half", L"半身"), 0, 0, 10, 10, kUpperBody);
    button(uiText(L"Full", L"全身"), 0, 0, 10, 10, kFullBody);
    addControl(window, state, L"STATIC", L"", SS_OWNERDRAW, 0, 0, 0, 10, 10, kPreview);
    button(uiText(L"Pause", L"暂停"), 0, 0, 10, 10, kPlay);
    const auto scrub = addControl(window, state, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY | WS_TABSTOP, 0, 0, 0, 10, 10, kScrub);
    const auto originalScrub = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(scrub, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(buttonProcedure)));
    SetPropW(scrub, L"Live2DOriginalButtonProc", reinterpret_cast<HANDLE>(originalScrub));
    label(uiText(L"0.00 / 3.00 s", L"0.00 / 3.00 秒"), 0, 0, 10, 10, kTime, true);
    label(uiText(L"Loading preview. The first load can take a few seconds...", L"正在加载预览，首次加载需要数秒…"), 0, 0, 10, 10, kStatus, true);
    label(uiText(L"Transition", L"过渡时长"), 0, 0, 10, 10, kTransitionTitle);
    label(uiText(L"Overshoot", L"回弹幅度"), 0, 0, 10, 10, kBounceTitle);
    label(uiText(L"Placement", L"添加位置"), 0, 0, 10, 10, kPlacementTitle);
    fillSettingCombo(addCombo(window, state, 0, 0, 10, 28, kTransition), state, kTransitionChoices, 5, state.transitionFrames);
    fillSettingCombo(addCombo(window, state, 0, 0, 10, 28, kBounceNone), state, kReboundChoices, 3, state.transitionCurve);
    fillSettingCombo(addCombo(window, state, 0, 0, 10, 28, kAppendClip), state, kPlacementChoices, 2, state.appendClip ? 1 : 0);
    button(uiText(L"Add to Timeline", L"添加到时间线"), 0, 0, 10, 10, IDOK);
    layoutDialog(window, state);
    SendMessageW(window, DM_SETDEFID, IDOK, 0);
    state.controlsReady = true;
    rebuildList(window, state, state.motions); rebuildList(window, state, state.expressions);
    state.lastTick = GetTickCount64();
    if (state.previewEnabled) {
        state.worker.start(state.pending.modelPath); submitPreview(window, state);
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
    const auto px = [&state](RECT rect) {
        return RECT{MulDiv(rect.left, state.dpi, 96), MulDiv(rect.top, state.dpi, 96),
            MulDiv(rect.right, state.dpi, 96), MulDiv(rect.bottom, state.dpi, 96)};
    };
    for (const auto& rect : {state.motionCard, state.expressionCard, state.previewCard,
            state.transitionCard, state.reboundCard, state.placementCard})
        if (rect.right > rect.left && rect.bottom > rect.top)
            rounded(dc, px(rect), kCardColor, kBorderColor, MulDiv(14, state.dpi, 96));
    for (const int id : {kSearchMotion, kSearchExpression})
        for (const auto& control : state.controlLayouts) if (GetDlgCtrlID(control.window) == id) {
            RECT well{control.x - 6, control.y - 4, control.x + control.width + 6, control.y + control.height + 4};
            rounded(dc, px(well), kInputColor, kBorderColor, MulDiv(9, state.dpi, 96));
        }
    for (const auto& control : state.controlLayouts) if (GetDlgCtrlID(control.window) == IDOK) {
        const auto pen = CreatePen(PS_SOLID, 1, kBorderColor);
        const auto previous = SelectObject(dc, pen);
        const int y = MulDiv(control.y - 10, state.dpi, 96);
        MoveToEx(dc, MulDiv(20, state.dpi, 96), y, nullptr);
        LineTo(dc, client.right - MulDiv(20, state.dpi, 96), y);
        SelectObject(dc, previous); DeleteObject(pen);
    }
    RestoreDC(dc, saved);
}

bool buttonChosen(int id, const DialogState& state) {
    return (id == kUpperBody && state.upperBody) || (id == kFullBody && !state.upperBody);
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

std::wstring cellLabel(const PickerList& list, int cell) {
    if (cell <= 0) return list.expression ? uiText(L"(Skip expression)", L"（不导入表情）") : uiText(L"(Skip motion)", L"（不导入动作）");
    return animationName(list.entries[list.visible[static_cast<size_t>(cell - 1)]].path);
}

void drawSearchHint(const DRAWITEMSTRUCT& item, const DialogState& state) {
    FillRect(item.hDC, &item.rcItem, state.cardBrush);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, kMutedColor);
    auto rect = item.rcItem;
    const auto old = SelectObject(item.hDC, state.font);
    const auto text = controlText(item.hwndItem);
    DrawTextW(item.hDC, text.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, old);
}

void drawListItem(const DRAWITEMSTRUCT& item, const DialogState& state) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto& list = item.CtlID == kExpressionList ? state.expressions : state.motions;
    FillRect(item.hDC, &item.rcItem, state.cardBrush);
    const int width = std::max(1, static_cast<int>(item.rcItem.right - item.rcItem.left));
    const int cells = pickerCells(list);
    const int padX = MulDiv(4, state.dpi, 96), padY = MulDiv(3, state.dpi, 96);
    for (int column = 0; column < kGridColumns; ++column) {
        const int cell = static_cast<int>(item.itemID) * kGridColumns + column;
        if (cell >= cells) break;
        RECT chip{item.rcItem.left + column * width / kGridColumns, item.rcItem.top,
            item.rcItem.left + (column + 1) * width / kGridColumns, item.rcItem.bottom};
        InflateRect(&chip, -padX, -padY);
        const bool selected = cell == 0 ? list.selected.empty()
            : list.entries[list.visible[static_cast<size_t>(cell - 1)]].path == list.selected;
        if (selected) rounded(item.hDC, chip, RGB(56, 49, 83), kAccentColor, MulDiv(8, state.dpi, 96));
        const int radius = MulDiv(4, state.dpi, 96);
        const int cx = chip.left + MulDiv(12, state.dpi, 96);
        const int cy = (chip.top + chip.bottom) / 2;
        const auto pen = CreatePen(PS_SOLID, 1, selected ? kAccentColor : kBorderColor);
        const auto dot = CreateSolidBrush(selected ? kAccentColor : kCardColor);
        const auto oldPen = SelectObject(item.hDC, pen), oldBrush = SelectObject(item.hDC, dot);
        Ellipse(item.hDC, cx - radius, cy - radius, cx + radius, cy + radius);
        SelectObject(item.hDC, oldPen); SelectObject(item.hDC, oldBrush); DeleteObject(pen); DeleteObject(dot);
        RECT textRect = chip;
        textRect.left = cx + radius + MulDiv(6, state.dpi, 96);
        textRect.right -= MulDiv(6, state.dpi, 96);
        const auto text = cellLabel(list, cell);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, cell == 0 ? kMutedColor : kTextColor);
        const auto old = SelectObject(item.hDC, state.font);
        DrawTextW(item.hDC, text.c_str(), -1, &textRect, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, old);
        if (selected && (item.itemState & ODS_FOCUS)) DrawFocusRect(item.hDC, &chip);
    }
}

void drawPreviewImage(const DRAWITEMSTRUCT& item, const DialogState& state) {
    FillRect(item.hDC, &item.rcItem, state.cardBrush);
    if (state.frame.bgra.empty()) {
        SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, kMutedColor);
        auto rect = item.rcItem; const auto old = SelectObject(item.hDC, state.font);
        DrawTextW(item.hDC, uiText(L"Preparing the character preview...", L"正在准备角色预览…"), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old); return;
    }
    const int destWidth = item.rcItem.right - item.rcItem.left, destHeight = item.rcItem.bottom - item.rcItem.top;
    const bool native = state.frame.width == destWidth && state.frame.height == destHeight;
    const float scale = native ? 1.f : std::min(static_cast<float>(destWidth) / state.frame.width,
        static_cast<float>(destHeight) / state.frame.height);
    const int width = native ? destWidth : static_cast<int>(state.frame.width * scale);
    const int height = native ? destHeight : static_cast<int>(state.frame.height * scale);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = state.frame.width;
    info.bmiHeader.biHeight = -state.frame.height; info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(item.hDC, native ? COLORONCOLOR : HALFTONE); SetBrushOrgEx(item.hDC, 0, 0, nullptr);
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

void drawSettingCombo(const DRAWITEMSTRUCT& item, const DialogState& state) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const bool closed = (item.itemState & ODS_COMBOBOXEDIT) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0 && !closed;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const auto fill = closed ? kInputColor : selected ? RGB(56, 49, 83) : kCardColor;
    const auto brush = CreateSolidBrush(fill);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    wchar_t text[128]{};
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(128, 126, 148) : kTextColor);
    auto rect = item.rcItem;
    InflateRect(&rect, -8, 0);
    const auto old = SelectObject(item.hDC, state.font);
    DrawTextW(item.hDC, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, old);
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
        if (message == WM_SIZE && state->controlsReady && wParam != SIZE_MINIMIZED) {
            layoutDialog(window, *state);
            RECT preview{};
            if (const auto control = GetDlgItem(window, kPreview)) GetClientRect(control, &preview);
            if (preview.right != state->frame.width || preview.bottom != state->frame.height)
                state->previewDirty = true;
            InvalidateRect(window, nullptr, FALSE);
            return TRUE;
        }
        if (message == WM_GETMINMAXINFO) {
            auto* limit = reinterpret_cast<MINMAXINFO*>(lParam);
            limit->ptMinTrackSize.x = MulDiv(860, state->dpi, 96);
            limit->ptMinTrackSize.y = MulDiv(560, state->dpi, 96);
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
            const bool input = id == kSearchMotion || id == kSearchExpression;
            const bool page = id == kHeading || id == kSubtitle;
            const bool muted = id == kStatus || id == kSubtitle || id == kTime || id == kMotionCount ||
                id == kExpressionCount || id == kMotionSearchHint || id == kExpressionSearchHint ||
                id == kMotionSelectedCaption || id == kExpressionSelectedCaption;
            SetTextColor(dc, muted ? kMutedColor : kTextColor);
            SetBkColor(dc, input ? kInputColor : page ? kBackgroundColor : kCardColor);
            return reinterpret_cast<INT_PTR>(input ? state->inputBrush : page ? state->backgroundBrush : state->cardBrush);
        }
        if (message == WM_MEASUREITEM) {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlType == ODT_LISTBOX) { measure->itemHeight = MulDiv(32, state->dpi, 96); return TRUE; }
            if (measure->CtlType == ODT_COMBOBOX) { measure->itemHeight = MulDiv(28, state->dpi, 96); return TRUE; }
        }
        if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
        if (message == WM_DESTROY) { KillTimer(window, kPreviewTimer); return TRUE; }
        if (message == WM_TIMER && wParam == kPreviewTimer) { tick(window, *state); return TRUE; }
        if (message == WM_DRAWITEM) {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (wParam == kPreview) drawPreview(item, *state);
            else if (wParam == kMotionSearchHint || wParam == kExpressionSearchHint) drawSearchHint(item, *state);
            else if (wParam == kTransition || wParam == kBounceNone || wParam == kAppendClip) drawSettingCombo(item, *state);
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
            if (id == kTransition && notification == CBN_SELCHANGE) {
                state->transitionFrames = selectedSetting(GetDlgItem(window, id), kTransitionChoices, 5, state->extraTransitionFrames);
                return TRUE;
            }
            if (id == kBounceNone && notification == CBN_SELCHANGE) {
                if (!state->modelValid || state->motions.selected.empty() || !IsWindowEnabled(GetDlgItem(window, id))) return TRUE;
                state->transitionCurve = selectedSetting(GetDlgItem(window, id), kReboundChoices, 3, state->transitionCurve);
                return TRUE;
            }
            if (id == kAppendClip && notification == CBN_SELCHANGE) {
                state->appendClip = selectedSetting(GetDlgItem(window, id), kPlacementChoices, 2, state->appendClip ? 1 : 0) != 0;
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
            for (auto* list : {&state->motions, &state->expressions}) {
                if (id == list->searchId && notification == EN_CHANGE && state->controlsReady) {
                    list->query = controlText(GetDlgItem(window, id));
                    rebuildList(window, *state, *list);
                    return TRUE;
                }
                if (id == list->listId && !state->rebuilding && (notification == LBN_SELCHANGE || notification == LBN_DBLCLK)) {
                    const auto control = GetDlgItem(window, id);
                    int cell = -1;
                    if (const auto stored = GetPropW(control, L"Live2DGridCell")) {
                        cell = static_cast<int>(reinterpret_cast<INT_PTR>(stored)) - 1;
                        RemovePropW(control, L"Live2DGridCell");
                        list->pressedCell = -1;
                    } else if (list->pressedCell >= 0) {
                        cell = list->pressedCell;
                        list->pressedCell = -1;
                    } else {
                        const auto row = SendMessageW(control, LB_GETCURSEL, 0, 0);
                        if (row < 0) return TRUE; // Filtering out the current selection never clears it.
                        cell = static_cast<int>(row) * kGridColumns + list->gridColumn;
                        if (cell >= pickerCells(*list)) cell = pickerCells(*list) - 1;
                    }
                    if (!applyListCell(*list, cell)) return TRUE;
                    InvalidateRect(control, nullptr, FALSE);
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
                state->transitionFrames = selectedSetting(GetDlgItem(window, kTransition), kTransitionChoices, 5, state->extraTransitionFrames);
                state->transitionCurve = selectedSetting(GetDlgItem(window, kBounceNone), kReboundChoices, 3, state->transitionCurve);
                state->appendClip = selectedSetting(GetDlgItem(window, kAppendClip), kPlacementChoices, 2, state->appendClip ? 1 : 0) != 0;
                if (!std::filesystem::is_regular_file(state->pending.modelPath))
                    throw std::runtime_error("The model file is missing. Restore its original path before importing.");
                state->result = prepareImport(*state, state->appendClip, state->transitionFrames);
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
    data.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    data.dialog.dwExtendedStyle = 0;
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
