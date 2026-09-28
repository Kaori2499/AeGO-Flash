// Exercise real native controls without opening a window, and the asynchronous
// preview scheduler with a renderer that deliberately blocks and fails.
#include "../src/Dialog.cpp"
#include <atomic>
#include <iostream>

namespace {
std::vector<l2dae::MotionEntry> fixture;
std::vector<l2dae::MotionEntry> expressionFixture;
bool rejectDuration = false;
bool rejectExpression = false;
std::atomic<int> durationCalls{0};
std::mutex rendererMutex;
std::condition_variable rendererCv;
bool blockRenderer = false;
bool rendererEntered = false;
std::vector<l2dae::RenderRequest> renderedRequests;
int checks = 0;
void require(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
    ++checks;
}
template<class Predicate> void waitUntil(Predicate predicate, const char* description) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(description);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++checks;
}
LRESULT CALLBACK testProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (l2dae::dialogProcedure(window, message, wParam, lParam)) return 0;
    return DefWindowProcW(window, message, wParam, lParam);
}
void selectRow(HWND window, int id, int row) {
    SendDlgItemMessageW(window, id, LB_SETCURSEL, row, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, LBN_SELCHANGE), 0);
}
}

namespace l2dae {
std::vector<MotionEntry> listMotions(const std::wstring& path) {
    if (path == L"invalid") throw std::runtime_error("invalid model fixture");
    return fixture;
}
std::vector<MotionEntry> listExpressions(const std::wstring&) { return expressionFixture; }
void validateExpression(const std::wstring&) {
    if (rejectExpression) throw std::runtime_error("invalid expression fixture");
}
double motionDuration(const std::wstring&) {
    ++durationCalls;
    if (rejectDuration) throw std::runtime_error("invalid motion fixture");
    return 2.75;
}
RenderResult render(const RenderRequest& request) {
    {
        std::unique_lock<std::mutex> lock(rendererMutex);
        renderedRequests.push_back(request);
        rendererEntered = true;
        rendererCv.notify_all();
        rendererCv.wait(lock, [] { return !blockRenderer; });
    }
    if (request.motionPath == L"invalid") throw std::runtime_error("preview error fixture");
    RenderResult result;
    result.width = result.height = 1;
    result.rgba = {64, 32, 16, 128}; // Premultiplied; composited onto checkerboard.
    return result;
}
}

int main() {
    HWND window = nullptr;
    try {
        for (int i = 0; i < 400; ++i) {
            wchar_t label[64]{};
            swprintf_s(label, L"Idle / Action %04d", i);
            fixture.push_back({label, L"C:\\模型测试\\motions\\动作" + std::to_wstring(i) + L".motion3.json"});
        }
        for (int i = 0; i < 320; ++i)
            expressionFixture.push_back({L"表情 / 微笑" + std::to_wstring(i),
                L"C:\\模型测试\\expressions\\微笑" + std::to_wstring(i) + L".exp3.json"});
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = testProcedure;
        windowClass.cbWndExtra = 128;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"Live2DHiddenCombinedDialogSmoke";
        require(RegisterClassW(&windowClass) != 0, "register hidden test window");
        l2dae::DialogState state;
        state.previewEnabled = false;
        require(state.transitionFrames == 30 && state.transitionCurve == 5, "first use defaults to thirty frames and pronounced rebound");
        for (const int curve : {3, 4, 5}) {
            l2dae::DialogState restored;
            l2dae::AnimationImportSelection preferences;
            preferences.transitionFrames = 47;
            preferences.transitionCurve = curve;
            l2dae::applyImportPreferences(restored, preferences);
            require(restored.transitionCurve == curve && restored.transitionFrames == 47,
                "dialog opening restores every confirmed rebound choice and supplied transition length");
            require(preferences.transitionCurve == curve && preferences.transitionFrames == 47,
                "opening a dialog never mutates the caller's saved preferences");
        }
        for (const int invalid : {-1, 0, 1, 2, 6, 100}) {
            l2dae::DialogState restored;
            l2dae::AnimationImportSelection preferences;
            preferences.transitionFrames = -1;
            preferences.transitionCurve = invalid;
            l2dae::applyImportPreferences(restored, preferences);
            require(restored.transitionCurve == 5 && restored.transitionFrames == 30,
                "missing or invalid preferences fall back to thirty frames and pronounced rebound");
        }
        state.pending.modelPath = L"C:\\模型测试\\test.model3.json";
        window = CreateWindowW(windowClass.lpszClassName, L"AeGO Flash dialog smoke", WS_OVERLAPPED,
            0, 0, 960, 616, nullptr, nullptr, windowClass.hInstance, nullptr);
        require(window != nullptr, "create hidden test window");
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(&state));
        l2dae::initialize(window, state);
        require(!IsWindowVisible(window), "test remains hidden");
        require(!IsWindowEnabled(GetDlgItem(window, IDOK)), "cannot import without a selection");
        require(l2dae::controlText(GetDlgItem(window, l2dae::kTransition)) == L"30", "default transition is thirty frames");
        require(state.appendClip, "motions append by default");
        require(state.transitionCurve == 5, "first dialog defaults to pronounced rebound");
        const int bounceButtons[]{l2dae::kBounceNone, l2dae::kBounceGentle, l2dae::kBounceStrong};
        const wchar_t* bounceNames[]{L"无", L"轻柔", L"明显"};
        for (int i = 0; i < 3; ++i) {
            const auto button = GetDlgItem(window, bounceButtons[i]);
            require(button && l2dae::controlText(button) == bounceNames[i], "all three rebound choices are labeled clearly");
            require(!IsWindowEnabled(button), "rebound choices stay disabled without a motion");
            require((GetWindowLongPtrW(button, GWL_STYLE) & WS_TABSTOP) != 0, "rebound choices participate in keyboard tab order");
            require(l2dae::buttonChosen(bounceButtons[i], state) == (i == 2), "only the pronounced rebound choice is selected initially");
        }
        require(GetNextDlgTabItem(window, GetDlgItem(window, l2dae::kTransition), FALSE) == GetDlgItem(window, l2dae::kAppendClip),
            "tab skips unavailable motion-only rebound controls");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(l2dae::kBounceGentle, BN_CLICKED), 0);
        require(state.transitionCurve == 5, "disabled rebound choices ignore queued click notifications");
        for (const auto& control : state.controlLayouts)
            require(l2dae::controlText(control.window).find(L"柔和回弹") == std::wstring::npos, "old explanatory rebound sentence is removed");
        require(state.upperBody, "preview opens in upper-body mode");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(l2dae::kAtPlayhead, BN_CLICKED), 0);
        require(!state.appendClip, "playhead placement is directly selectable");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(l2dae::kAppendClip, BN_CLICKED), 0);
        require(state.appendClip, "append placement can be restored");
        state.seconds = 1.25;
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(l2dae::kFullBody, BN_CLICKED), 0);
        require(!state.upperBody && state.seconds == 1.25, "full-body toggle preserves preview time");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(l2dae::kUpperBody, BN_CLICKED), 0);
        require(state.upperBody && state.seconds == 1.25, "upper-body toggle preserves preview time");
        SendMessageW(window, l2dae::kSeekPreview, 500, 0);
        require(!state.playing && state.seconds == 1.5, "scrub pauses playback and seeks the frame");
        SendMessageW(GetDlgItem(window, l2dae::kScrub), WM_KEYDOWN, VK_HOME, 0);
        require(state.seconds == 0, "keyboard Home seeks preview start");
        SendMessageW(GetDlgItem(window, l2dae::kScrub), WM_KEYDOWN, VK_RIGHT, 0);
        require(std::abs(state.seconds - .03) < .00001, "keyboard arrow adjusts preview time");
        require((GetWindowLongPtrW(GetDlgItem(window, IDOK), GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW,
            "default primary action retains custom appearance");
        require(l2dae::controlText(GetDlgItem(window, IDOK)) == L"添加到时间线", "combined import action clearly labeled");
        state.motions.entries = fixture;
        state.expressions.entries = expressionFixture;
        state.modelValid = true;
        l2dae::rebuildList(window, state, state.motions);
        l2dae::rebuildList(window, state, state.expressions);
        require(SendDlgItemMessageW(window, l2dae::kMotionList, LB_GETCOUNT, 0, 0) == 401, "all motions loaded with explicit none row");
        require(SendDlgItemMessageW(window, l2dae::kExpressionList, LB_GETCOUNT, 0, 0) == 321, "all expressions loaded independently");
        require(state.motions.selected.empty() && state.expressions.selected.empty(), "initial lists do not import anything implicitly");
        selectRow(window, l2dae::kMotionList, 124);
        selectRow(window, l2dae::kExpressionList, 24);
        require(state.motions.selected == fixture[123].path && state.expressions.selected == expressionFixture[23].path,
            "motion and expression may be selected simultaneously");
        require(IsWindowEnabled(GetDlgItem(window, IDOK)), "combined selection enables import");
        for (const int id : bounceButtons)
            require(IsWindowEnabled(GetDlgItem(window, id)), "choosing a motion enables its rebound controls");
        require(GetNextDlgTabItem(window, GetDlgItem(window, l2dae::kTransition), FALSE) == GetDlgItem(window, l2dae::kBounceNone) &&
            GetNextDlgTabItem(window, GetDlgItem(window, l2dae::kBounceNone), FALSE) == GetDlgItem(window, l2dae::kBounceGentle) &&
            GetNextDlgTabItem(window, GetDlgItem(window, l2dae::kBounceGentle), FALSE) == GetDlgItem(window, l2dae::kBounceStrong),
            "rebound choices follow the transition duration in keyboard tab order");
        for (int i = 0; i < 3; ++i) {
            SendDlgItemMessageW(window, bounceButtons[i], BM_CLICK, 0, 0);
            require(state.transitionCurve == i + 3, "clicking each rebound segment selects its curve");
            for (int j = 0; j < 3; ++j)
                require(l2dae::buttonChosen(bounceButtons[j], state) == (i == j), "exactly one rebound segment receives selected colors");
            const auto selected = l2dae::prepareImport(state, true, 15);
            require(selected.transitionCurve == i + 3 && selected.motion.transitionCurve == i + 3 && selected.expression.transitionCurve == 0,
                "each rebound choice reaches the motion while paired expression fades remain unchanged");
        }
        SendDlgItemMessageW(window, l2dae::kBounceStrong, WM_KEYDOWN, VK_RIGHT, 0);
        require(state.transitionCurve == 3, "right arrow wraps from strong to no rebound");
        SendDlgItemMessageW(window, l2dae::kBounceNone, WM_KEYDOWN, VK_LEFT, 0);
        require(state.transitionCurve == 5, "left arrow wraps from no rebound to strong");
        SendDlgItemMessageW(window, l2dae::kBounceStrong, WM_KEYDOWN, VK_LEFT, 0);
        require(state.transitionCurve == 4, "keyboard arrows select the middle rebound choice");
        require((SendDlgItemMessageW(window, l2dae::kBounceGentle, WM_GETDLGCODE, 0, 0) & DLGC_WANTARROWS) != 0,
            "dialog navigation delivers rebound arrow keys to the control");
        SendDlgItemMessageW(window, l2dae::kBounceNone, BM_CLICK, 0, 0);
        SetDlgItemTextW(window, l2dae::kSearchMotion, L"action 0123");
        require(SendDlgItemMessageW(window, l2dae::kMotionList, LB_GETCOUNT, 0, 0) == 2, "case-insensitive declared name search");
        require(SendDlgItemMessageW(window, l2dae::kMotionList, LB_GETCURSEL, 0, 0) == 1, "filtered motion remains selected");
        require(SendDlgItemMessageW(window, l2dae::kExpressionList, LB_GETCOUNT, 0, 0) == 321, "motion search leaves expression list unchanged");
        SetDlgItemTextW(window, l2dae::kSearchMotion, L"no-match");
        require(SendDlgItemMessageW(window, l2dae::kMotionList, LB_GETCURSEL, 0, 0) == LB_ERR, "hidden selection is not silently cleared");
        selectRow(window, l2dae::kMotionList, -1);
        require(state.motions.selected == fixture[123].path, "missing list selection leaves selected path intact");
        require(l2dae::controlText(GetDlgItem(window, l2dae::kMotionPath)) == L"动作123", "selected name remains visible without extension");
        SetDlgItemTextW(window, l2dae::kSearchExpression, L"微笑23.exp3");
        require(SendDlgItemMessageW(window, l2dae::kExpressionList, LB_GETCOUNT, 0, 0) == 2, "Unicode expression path search");
        auto imported = l2dae::prepareImport(state, true, state.transitionFrames);
        require(imported.hasMotion && imported.hasExpression, "both selection flags preserved");
        require(imported.motion.label == L"动作123" && imported.expression.label == L"微笑23", "timeline names use imported file names");
        require(imported.motion.slot == 1 && imported.expression.slot == 1, "separate asset banks begin at stable ID one");
        require(imported.motion.duration == 2.75 && imported.expression.duration == 2.75, "paired expression matches motion duration");
        require(imported.motion.append && !imported.expression.append, "paired expression is independently aligned by timeline import");
        require(imported.transitionFrames == 30 && imported.motion.transitionFrames == 30 && imported.expression.transitionFrames == 30,
            "default thirty-frame transition is present on both clips");
        require(imported.transitionCurve == 3 && imported.motion.transitionCurve == 3 && imported.expression.transitionCurve == 0,
            "chosen curve affects the motion only, preserving independent expression fades");
        for (const int invalid : {-1, 0, 1, 2, 6}) {
            state.transitionCurve = invalid;
            bool rejectedCurve = false;
            try { (void)l2dae::prepareImport(state, true, 15); } catch (...) { rejectedCurve = true; }
            require(rejectedCurve, "new imports reject legacy or invalid dialog curve states before commit");
        }
        state.transitionCurve = 3;
        require(imported.motion.selection.motionPaths == imported.selection.motionPaths &&
            imported.motion.selection.expressionPaths == imported.selection.expressionPaths &&
            imported.expression.selection.motionPaths == imported.selection.motionPaths &&
            imported.expression.selection.expressionPaths == imported.selection.expressionPaths, "both clips carry atomic combined bank");
        require(state.pending.motionPaths.empty() && state.pending.expressionPaths.empty(), "preparation never mutates caller banks");
        rejectExpression = true;
        bool rejected = false;
        try { (void)l2dae::prepareImport(state, true, 15); } catch (...) { rejected = true; }
        rejectExpression = false;
        require(rejected && state.pending.motionPaths.empty() && state.pending.expressionPaths.empty(), "invalid expression cannot partially commit valid motion");
        rejectDuration = true;
        rejected = false;
        try { (void)l2dae::prepareImport(state, true, 15); } catch (...) { rejected = true; }
        rejectDuration = false;
        require(rejected && state.pending.motionPaths.empty(), "invalid duration does not allocate asset IDs");

        // Import more than both former limits, save each result, and then reuse
        // every entry. IDs must remain stable without replacing existing clips.
        for (int i = 0; i < 128; ++i) {
            state.motions.selected = fixture[i].path;
            state.expressions.selected = expressionFixture[i].path;
            imported = l2dae::prepareImport(state, true, i % 30);
            require(imported.motion.slot == i + 1 && imported.expression.slot == i + 1, "dynamic asset IDs grow past the former cap");
            state.pending = imported.selection;
        }
        const auto saved = state.pending;
        require(saved.motionPaths.size() == 128 && saved.expressionPaths.size() == 128, "one model holds 128 different motions and expressions");
        for (int i = 127; i >= 0; --i) {
            state.motions.selected = fixture[i].path;
            state.expressions.selected = expressionFixture[i].path;
            imported = l2dae::prepareImport(state, false, 0);
            require(imported.motion.slot == i + 1 && imported.expression.slot == i + 1, "reimport uses persistent asset IDs");
            require(imported.selection.motionPaths == saved.motionPaths && imported.selection.expressionPaths == saved.expressionPaths,
                "reimport preserves all existing clips");
        }
        state.pending.motionPaths[7].clear();
        state.motions.selected = fixture[300].path;
        imported = l2dae::prepareImport(state, false, 0);
        require(imported.motion.slot == 8 && imported.selection.motionPaths.size() == 128, "legacy empty bank entries reused without renumbering");
        state.pending = saved;
        state.motions.selected = L"D:\\外部动作\\新动作.motion3.json";
        state.expressions.selected = L"D:\\外部表情\\新表情.exp3.json";
        l2dae::rebuildList(window, state, state.motions);
        l2dae::rebuildList(window, state, state.expressions);
        const auto motionCount = state.motions.entries.size();
        l2dae::rebuildList(window, state, state.motions);
        require(state.motions.entries.size() == motionCount, "external entries are never duplicated by filtering");
        imported = l2dae::prepareImport(state, false, 7);
        require(imported.motion.slot == 129 && imported.expression.slot == 129, "external entries have no old eight-item cap");
        require(imported.motion.label == L"新动作" && imported.expression.label == L"新表情", "external Unicode names strip compound extension");
        SendDlgItemMessageW(window, l2dae::kBounceStrong, BM_CLICK, 0, 0);
        require(state.transitionCurve == 5, "motion rebound choice can be changed before clearing the motion");
        selectRow(window, l2dae::kMotionList, 0);
        imported = l2dae::prepareImport(state, true, 15);
        require(!imported.hasMotion && imported.hasExpression && imported.expression.duration == 3.0, "expression-only import remains available");
        require(imported.transitionCurve == 5 && imported.expression.transitionCurve == 0,
            "expression-only output preserves the UI preference without applying it to the expression clip");
        for (const int id : bounceButtons)
            require(!IsWindowEnabled(GetDlgItem(window, id)), "expression-only selection disables every rebound choice");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(l2dae::kBounceGentle, BN_CLICKED), 0);
        SendDlgItemMessageW(window, l2dae::kBounceStrong, WM_KEYDOWN, VK_LEFT, 0);
        require(state.transitionCurve == 5, "disabled rebound controls ignore both queued clicks and key presses");
        state.motions.selected = fixture[2].path;
        selectRow(window, l2dae::kExpressionList, 0);
        imported = l2dae::prepareImport(state, false, 15);
        require(imported.hasMotion && !imported.hasExpression && !imported.motion.append, "motion-only import at playhead remains available");
        require(imported.motion.transitionCurve == 5, "reselecting a motion restores the current dialog rebound choice");
        selectRow(window, l2dae::kMotionList, 0);
        rejected = false;
        try { (void)l2dae::prepareImport(state, true, 15); } catch (...) { rejected = true; }
        require(rejected && !IsWindowEnabled(GetDlgItem(window, IDOK)), "empty combined selection cannot import");
        state.motions.selected = fixture[0].path;
        rejected = false;
        try { (void)l2dae::prepareImport(state, true, 100001); } catch (...) { rejected = true; }
        require(rejected, "dialog rejects values above the host transition range before commit");
        require(l2dae::animationName(L"C:\\x\\MOTION.MOTION3.JSON") == L"MOTION", "extension normalization ignores case");
        l2dae::RenderResult neutral;
        neutral.width = 384; neutral.height = 448;
        neutral.rgba.resize(static_cast<size_t>(neutral.width) * neutral.height * 4);
        for (int y = 75; y < 421; ++y) for (int x = 120; x < 270; ++x)
            neutral.rgba[(static_cast<size_t>(y) * neutral.width + x) * 4 + 3] = 255;
        neutral.rgba[3] = 8; // Nearly transparent speck must not distort framing.
        const auto bounds = l2dae::alphaBounds(neutral);
        require(bounds.valid && bounds.left == 120 && bounds.right == 270 && bounds.top == 75 && bounds.bottom == 421,
            "calibration measures visible character and ignores transparent margins");
        l2dae::RenderRequest portrait; portrait.width = 384; portrait.height = 448;
        const auto upper = l2dae::framePreview(portrait, bounds, true);
        const auto full = l2dae::framePreview(portrait, bounds, false);
        const auto screenY = [](float y, const l2dae::RenderRequest& r) { return r.height * .5f + (y - r.height * .5f) * r.scale + r.offsetY; };
        const auto screenX = [](float x, const l2dae::RenderRequest& r) { return r.width * .5f + (x - r.width * .5f) * r.scale + r.offsetX; };
        require(upper.scale > full.scale, "upper-body mode zooms relative to full body");
        require(std::abs(screenY(75, upper) - 448 * .04f) < .001, "upper-body framing retains four-percent headroom");
        require(std::abs(screenY(75 + 346 * .46f, upper) - 448 * .96f) < .001, "upper forty-six percent of character fills preview height");
        require(std::abs(screenX(195, upper) - 192) < .001 && std::abs(screenX(195, full) - 192) < .001,
            "both framing modes center a model with asymmetric canvas margins");
        require(screenY(75, full) >= 0 && screenY(421, full) <= 448 && screenX(120, full) >= 0 && screenX(270, full) <= 384,
            "full-body framing preserves the entire visible character");
        require(l2dae::framePreview(portrait, {}, true).scale == 1, "blank neutral model falls back to safe fitted canvas");
        for (const int dpi : {96, 144, 192}) for (const auto available : {std::pair<int,int>{960, 680}, {1366, 768}, {1920, 1080}}) {
            const int fitted = l2dae::fittedDpi(dpi, available.first, available.second);
            require(fitted <= dpi && MulDiv(l2dae::kLayoutWidth, fitted, 96) <= available.first - 32 &&
                MulDiv(l2dae::kLayoutHeight, fitted, 96) <= available.second - 56, "dialog fits monitor work area at multiple DPIs");
        }
        require(l2dae::controlText(GetDlgItem(window, l2dae::kMotionSearchHint)) == L"搜索" &&
            l2dae::controlText(GetDlgItem(window, l2dae::kExpressionSearchHint)) == L"搜索", "search fields remain identifiable without OS cue-banner support");
        const auto selectedBeforeDpi = state.motions.selected, queryBeforeDpi = state.motions.query;
        const auto fontBeforeDpi = state.font;
        const double timeBeforeDpi = state.seconds;
        RECT suggested{30, 30, 1200, 800};
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
        require(state.font != fontBeforeDpi && state.motions.selected == selectedBeforeDpi && state.motions.query == queryBeforeDpi &&
            state.seconds == timeBeforeDpi, "DPI reflow replaces fonts without changing selection search or playback");
        for (const auto& control : state.controlLayouts) {
            RECT actual{}; GetWindowRect(control.window, &actual); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&actual), 2);
            require(actual.left == MulDiv(control.x, state.dpi, 96) && actual.top == MulDiv(control.y, state.dpi, 96) &&
                actual.right - actual.left == MulDiv(control.width, state.dpi, 96), "DPI reflow uses the same logical geometry for every native control");
        }
        const auto textDc = GetDC(window);
        for (const auto& control : state.controlLayouts) {
            const int id = GetDlgCtrlID(control.window);
            const auto text = l2dae::controlText(control.window);
            if ((id < l2dae::kBounceNone || id > l2dae::kTransitionUnit) && id != l2dae::kAppendClip &&
                id != l2dae::kAtPlayhead && id != l2dae::kTransition) continue;
            const auto oldFont = SelectObject(textDc, reinterpret_cast<HFONT>(SendMessageW(control.window, WM_GETFONT, 0, 0)));
            SIZE textSize{};
            const bool measured = GetTextExtentPoint32W(textDc, text.c_str(), static_cast<int>(text.size()), &textSize) != 0;
            SelectObject(textDc, oldFont);
            RECT box{}; GetClientRect(control.window, &box);
            require(measured && textSize.cx < box.right && textSize.cy <= box.bottom,
                "transition section Chinese captions fit their native controls after DPI reflow");
            require(reinterpret_cast<HFONT>(SendMessageW(control.window, WM_GETFONT, 0, 0)) == state.font,
                "all settings titles input units and buttons use the same font");
            const bool durationCard = id == l2dae::kTransitionTitle || id == l2dae::kTransition || id == l2dae::kTransitionUnit;
            const bool placementCard = id == l2dae::kPlacementTitle || id == l2dae::kAppendClip || id == l2dae::kAtPlayhead;
            const RECT card = durationCard ? RECT{24, 458, 222, 538} : placementCard ? RECT{24, 550, 552, 628} : RECT{234, 458, 552, 538};
            require(control.x >= card.left + 16 && control.x + control.width <= card.right - 16 &&
                control.y >= card.top + 10 && control.y + control.height <= card.bottom - 10,
                "settings controls remain padded inside their own titled card");
            if ((id >= l2dae::kBounceNone && id <= l2dae::kBounceStrong) || id == l2dae::kAppendClip || id == l2dae::kAtPlayhead)
                require(control.height == 32, "rebound and placement buttons share a consistent height");
        }
        ReleaseDC(window, textDc);
        require(l2dae::controlText(GetDlgItem(window, l2dae::kTransitionTitle)) == L"过渡时长" &&
            l2dae::controlText(GetDlgItem(window, l2dae::kBounceTitle)) == L"回弹幅度" &&
            l2dae::controlText(GetDlgItem(window, l2dae::kPlacementTitle)) == L"添加位置",
            "three separate settings sections have clear permanent titles");
        for (const auto& control : state.controlLayouts) {
            const int id = GetDlgCtrlID(control.window);
            if (id == l2dae::kMotionList || id == l2dae::kExpressionList)
                require(control.height >= 4 * 32 && control.y + control.height <= 362,
                    "shortened motion and expression lists still show at least four rows above settings");
            if (id == l2dae::kPreview)
                require(control.width == 406 && control.height == 400, "settings redesign preserves the large character preview");
        }
        state.result.transitionCurve = 4;
        state.result.transitionFrames = 777;
        const auto beforeCancel = state.pending;
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        require(state.result.transitionCurve == 4 && state.result.transitionFrames == 777 &&
            state.pending.motionPaths == beforeCancel.motionPaths && state.pending.expressionPaths == beforeCancel.expressionPaths,
            "cancel leaves the accepted result and caller banks untouched despite changed rebound controls");
        l2dae::DialogState reopened;
        l2dae::AnimationImportSelection savedPreferences;
        savedPreferences.transitionCurve = 4;
        savedPreferences.transitionFrames = 47;
        l2dae::applyImportPreferences(reopened, savedPreferences);
        require(reopened.transitionCurve == 4 && reopened.transitionFrames == 47,
            "after cancellation a later dialog uses the last confirmed preference supplied by the host");
        DestroyWindow(window);
        window = nullptr;

        l2dae::PreviewWorker worker;
        worker.start(L"model");
        l2dae::PreviewWorker::Library library;
        waitUntil([&] { return worker.takeLibrary(library); }, "preview metadata loaded asynchronously");
        require(library.motions.size() == 400 && library.expressions.size() == 320, "preview returns both full libraries");
        l2dae::RenderRequest request;
        request.modelPath = L"model";
        request.motionPath = fixture[0].path;
        request.expressionPathA = expressionFixture[0].path;
        request.expressionWeightA = 1;
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            blockRenderer = true;
            rendererEntered = false;
        }
        const auto durationsBeforeRender = durationCalls.load();
        worker.submit(request, 1);
        waitUntil([] { std::lock_guard<std::mutex> lock(rendererMutex); return rendererEntered; }, "first preview began");
        for (int i = 2; i <= 100; ++i) { request.seconds = i; worker.submit(request, static_cast<std::uint64_t>(i)); }
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            blockRenderer = false;
            rendererCv.notify_all();
        }
        l2dae::PreviewWorker::Result frame;
        waitUntil([&] { return worker.takeResult(frame); }, "latest preview finishes");
        require(frame.generation == 100 && frame.error.empty(), "old selections are discarded after rapid scrubbing");
        require(frame.duration == 2.75 && frame.width == 1 && frame.height == 1, "preview publishes motion duration and rendered image");
        require(durationCalls.load() - durationsBeforeRender == 1, "preview reuses motion metadata between consecutive frames");
        require(frame.bgra == std::vector<std::uint8_t>({35, 51, 83, 255}), "premultiplied RGBA correctly composites to dark checkerboard BGRA");
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            require(renderedRequests.size() == 2 && renderedRequests.back().seconds == 100, "worker queue stays bounded and coalesces obsolete frames");
            require(renderedRequests.back().expressionPathA == expressionFixture[0].path, "motion preview includes selected expression");
        }
        request.motionPath = L"invalid";
        worker.submit(request, 101);
        waitUntil([&] { return worker.takeResult(frame); }, "preview error delivered");
        require(!frame.error.empty() && frame.generation == 101, "render failure is isolated to the preview result");
        request.motionPath.clear();
        worker.submit(request, 102);
        waitUntil([&] { return worker.takeResult(frame); }, "preview recovers after error");
        require(frame.error.empty() && frame.duration == 3.0, "expression-only preview recovers and uses three-second loop");
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            blockRenderer = true;
            rendererEntered = false;
        }
        worker.submit(request, 103);
        waitUntil([] { std::lock_guard<std::mutex> lock(rendererMutex); return rendererEntered; }, "final render begins before dialog close");
        std::atomic<bool> stopped{false};
        std::thread closer([&] { worker.stop(); stopped = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool didWait = !stopped.load();
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            blockRenderer = false;
            rendererCv.notify_all();
        }
        closer.join();
        require(didWait && stopped.load(), "dialog close drains in-flight render before destroying state");
        require(!worker.takeResult(frame), "cancelled in-flight frame never publishes after close");
        l2dae::PreviewWorker invalid;
        invalid.start(L"invalid");
        waitUntil([&] { return invalid.takeLibrary(library); }, "metadata failure returns asynchronously");
        require(!library.error.empty(), "invalid model error reported without crashing UI");
        invalid.stop();
        l2dae::PreviewWorker calibrated;
        calibrated.start(L"model");
        waitUntil([&] { return calibrated.takeLibrary(library); }, "calibrated preview metadata ready");
        request.width = 384; request.height = 448;
        request.motionPath = fixture[1].path;
        request.expressionPathA = expressionFixture[1].path;
        request.breathingEnabled = request.autoBlinkEnabled = request.lipSyncEnabled = true;
        size_t priorCalls;
        { std::lock_guard<std::mutex> lock(rendererMutex); priorCalls = renderedRequests.size(); }
        calibrated.submit(request, 1, true);
        waitUntil([&] { return calibrated.takeResult(frame); }, "neutral calibration and zoomed preview complete");
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            require(renderedRequests.size() == priorCalls + 2, "first portrait frame calibrates once then renders selection");
            const auto& first = renderedRequests[priorCalls];
            require(first.motionPath.empty() && first.expressionPathA.empty() && !first.breathingEnabled &&
                !first.autoBlinkEnabled && !first.lipSyncEnabled && first.scale == 1, "neutral calibration excludes motions expressions and ambient effects");
            require(renderedRequests.back().motionPath == request.motionPath && renderedRequests.back().expressionPathA == request.expressionPathA,
                "upper-body preview retains selected animation and expression");
        }
        calibrated.submit(request, 2, false);
        waitUntil([&] { return calibrated.takeResult(frame); }, "full-body toggle renders");
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            require(renderedRequests.size() == priorCalls + 3, "framing toggle reuses cached neutral bounds");
        }
        calibrated.stop();
        std::cout << "PASS " << checks << " checks: combined picker, dynamic assets, preview scheduling and cleanup\n";
        return 0;
    } catch (const std::exception& e) {
        if (window) DestroyWindow(window);
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
