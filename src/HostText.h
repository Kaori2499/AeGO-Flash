#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace l2dae {

struct HostVersion {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    unsigned build = 0;
    bool valid = false;
};

// This is the application version from either PluginDataEntryFunction entry
// point, not PF_InData.version (the latter is the effect API version, 13.x).
inline HostVersion parseHostVersion(std::string_view text) noexcept {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    HostVersion result;
    unsigned* parts[]{&result.major, &result.minor, &result.patch, &result.build};
    size_t at = 0;
    for (size_t part = 0; part < 4; ++part) {
        const size_t start = at;
        unsigned value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            const unsigned digit = static_cast<unsigned>(text[at++] - '0');
            if (value > (65535u - digit) / 10) return {};
            value = value * 10 + digit;
        }
        if (at == start) return {};
        *parts[part] = value;
        if (at == text.size()) break;
        if (text[at] != '.') {
            // Adobe commonly adds "x87" or " (Build 87)" after the version.
            if (!space(text[at]) && text[at] != 'x' && text[at] != 'X' && text[at] != '(') return {};
            break;
        }
        if (part == 3) return {};
        ++at;
    }
    result.valid = result.major != 0;
    return result;
}
inline HostVersion parseHostVersion(const char* text) noexcept {
    return parseHostVersion(text ? std::string_view(text) : std::string_view{});
}

inline bool isSimplifiedChineseHost(std::string_view language) noexcept {
    if (language.size() != 5) return false;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; };
    return lower(language[0]) == 'z' && lower(language[1]) == 'h' &&
        (language[2] == '_' || language[2] == '-') && lower(language[3]) == 'c' && lower(language[4]) == 'n';
}

// Adobe documents the Unicode UI rollout as AE 26.0. AE 26.3 UTF-8 behavior
// has also been observed locally. The 26.0 boundary is a compatibility policy
// inferred from that rollout, pending real-host coverage of 26.0/26.1 and AE22.
// https://helpx.adobe.com/after-effects/desktop/get-started/language-support/improved-ui-language-support.html
// https://community.adobe.com/questions-534/improved-ui-language-support-in-ae-beta-25-4x5-314462
// Earlier SDK localization guidance specifies the application's legacy locale
// and Windows CP_OEMCP. Chinese AE targets CP936 unless the OS explicitly uses
// UTF-8 for legacy programs. Other legacy locales retain the supplied OEM page.
// This function owns no state; the caller freezes its result before UI setup.
inline UINT hostTextCodePage(HostVersion version, std::string_view language = {}, UINT legacyOemCodePage = GetOEMCP()) noexcept {
    if (!version.valid || version.major >= 26) return CP_UTF8;
    if (legacyOemCodePage == CP_OEMCP) legacyOemCodePage = GetOEMCP();
    if (legacyOemCodePage == CP_ACP) legacyOemCodePage = GetACP();
    if (legacyOemCodePage == CP_UTF8) return CP_UTF8;
    if (isSimplifiedChineseHost(language)) return 936;
    return legacyOemCodePage;
}

// Labels default to lossless conversion: a missing character is an error, not
// a silent '?' or best-fit substitution. Error fields can explicitly permit a
// replacement when a legacy code page cannot represent a diagnostic character;
// Unicode dialogs remain available for the complete original text.
inline std::string encodeHostText(const std::wstring& text, UINT codePage, bool allowReplacement = false) {
    if (text.empty()) return {};
    const auto fail = [] { throw std::runtime_error("Unable to encode the effect UI text."); };
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) || !IsValidCodePage(codePage)) fail();
    const DWORD flags = codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL replaced = FALSE;
    BOOL* replacement = codePage == CP_UTF8 ? nullptr : &replaced;
    const int size = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, replacement);
    if (!size || (!allowReplacement && replaced)) fail();
    std::string output(static_cast<size_t>(size), '\0');
    replaced = FALSE;
    if (!WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()), output.data(), size, nullptr, replacement) ||
        (!allowReplacement && replaced)) fail();
    return output;
}

inline std::string encodeHostUtf8(const std::string& text, UINT codePage, bool allowReplacement = false) {
    if (text.empty()) return {};
    const auto fail = [] { throw std::runtime_error("Unable to encode the effect UI text."); };
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) fail();
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) fail();
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size)) fail();
    return encodeHostText(wide, codePage, allowReplacement);
}

// Input must be produced by encodeHostText/encodeHostUtf8 or otherwise valid in
// the selected code page. Never split a UTF-8 code point or a legacy DBCS pair.
inline size_t copyHostText(char* destination, size_t capacity, std::string_view text, UINT codePage) noexcept {
    if (!destination || !capacity) return 0;
    const size_t limit = capacity - 1;
    size_t count = 0;
    while (count < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[count]);
        size_t width = 1;
        if (codePage == CP_UTF8) {
            if ((first & 0xe0) == 0xc0) width = 2;
            else if ((first & 0xf0) == 0xe0) width = 3;
            else if ((first & 0xf8) == 0xf0) width = 4;
        } else if (IsDBCSLeadByteEx(codePage, first)) width = 2;
        if (width > text.size() - count || width > limit - count) break;
        count += width;
    }
    if (count) std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
    return count;
}

} // namespace l2dae
