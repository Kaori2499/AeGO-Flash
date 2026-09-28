#pragma once
#include "HostText.h"
#include <atomic>
#include <string_view>

namespace l2dae {

// Effect-panel strings still have to be encoded for the host code page.
// Simplified Chinese is the only translated UI; every other AE language uses
// ASCII English, which survives CP1252, CP936 and CP437 unchanged.
enum class UiLanguage : unsigned { English = 0, SimplifiedChinese = 1 };

inline std::atomic<unsigned>& uiLanguageStorage() noexcept {
    // Tests and hosts that never report a language keep the original Chinese UI.
    static std::atomic<unsigned> language{static_cast<unsigned>(UiLanguage::SimplifiedChinese)};
    return language;
}

inline UiLanguage uiLanguage() noexcept {
    return static_cast<UiLanguage>(uiLanguageStorage().load(std::memory_order_relaxed));
}

inline bool simplifiedChineseUi() noexcept {
    return uiLanguage() == UiLanguage::SimplifiedChinese;
}

// An empty tag means language detection did not run. Leave the current choice
// alone so a missing suite cannot flip an already-localized session.
inline void setUiLanguageFromHost(std::string_view language) noexcept {
    if (language.empty()) return;
    uiLanguageStorage().store(static_cast<unsigned>(isSimplifiedChineseHost(language)
        ? UiLanguage::SimplifiedChinese : UiLanguage::English), std::memory_order_relaxed);
}

inline const wchar_t* uiText(const wchar_t* english, const wchar_t* chinese) noexcept {
    return simplifiedChineseUi() ? chinese : english;
}

} // namespace l2dae
