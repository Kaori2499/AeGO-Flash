#include "HostText.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void expect(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
template<class F> void reject(F&& action, const char* reason) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, reason);
}
std::wstring decode(const std::string& text, UINT codePage) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    expect(size > 0, "Encoded output has a broken character boundary");
    std::wstring result(static_cast<size_t>(size), L'\0');
    expect(MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size) == size,
        "Encoded output cannot be decoded consistently");
    return result;
}
void boundaries(const std::wstring& original, UINT codePage) {
    const auto bytes = l2dae::encodeHostText(original, codePage);
    for (size_t capacity = 0; capacity <= bytes.size() + 2; ++capacity) {
        std::vector<char> field(capacity + 2, '!');
        const size_t count = l2dae::copyHostText(field.data(), capacity, bytes, codePage);
        expect(field[capacity] == '!' && field[capacity + 1] == '!', "Copy wrote outside the requested field");
        if (!capacity) { expect(count == 0 && field[0] == '!', "Zero-capacity field was modified"); continue; }
        expect(count < capacity && field[count] == '\0', "Field has no in-bounds terminator");
        expect(bytes.compare(0, count, field.data(), count) == 0, "Copy changed the encoded bytes");
        const auto wide = decode(std::string(field.data(), count), codePage);
        expect(original.compare(0, wide.size(), wide) == 0, "Truncation changed characters instead of retaining a prefix");
        expect(l2dae::encodeHostText(wide, codePage) == std::string(field.data(), count), "Truncation split a multi-byte character");
    }
}
}

int main() {
    try {
        for (const auto* version : {"22.0", "22.6.4", "23.0", "23.5", "24.6", "25.0", "25.6.1"}) {
            const auto parsed = l2dae::parseHostVersion(version);
            expect(parsed.valid, "Legacy application version was not parsed");
            expect(l2dae::hostTextCodePage(parsed, "zh_CN", 936) == 936, "AE22-25 Chinese labels must use the legacy page");
            expect(l2dae::hostTextCodePage(parsed, "zh_CN", 437) == 936, "Chinese application locale must select CP936");
            expect(l2dae::hostTextCodePage(parsed, "en_US", 437) == 437, "Other legacy locale must retain its OEM page");
            expect(l2dae::hostTextCodePage(parsed, "zh_CN", CP_UTF8) == CP_UTF8, "Legacy UTF-8 system mode must remain UTF-8");
        }
        for (const auto* version : {"26.0", "26.1", "26.2", "26.3.0x87", "26.5.0.12", "27.0"})
            expect(l2dae::hostTextCodePage(l2dae::parseHostVersion(version), "zh_CN", 936) == CP_UTF8, "AE26+ boundary must use UTF-8");
        const auto build = l2dae::parseHostVersion(" 26.3.1.87 ");
        expect(build.valid && build.major == 26 && build.minor == 3 && build.patch == 1 && build.build == 87, "Dotted host version components changed");
        const auto suffix = l2dae::parseHostVersion("22.6.4 (Build 2)");
        expect(suffix.valid && suffix.major == 22 && suffix.minor == 6 && suffix.patch == 4, "Adobe build suffix lost application version");
        for (const auto* version : {"", "After Effects 26.3", "26.", "26..3", "26a", "-22.0", "0.0", "999999999999.1", "26.3.0.1.2"}) {
            const auto parsed = l2dae::parseHostVersion(version);
            expect(!parsed.valid, "Invalid application version was accepted");
            expect(l2dae::hostTextCodePage(parsed, "zh_CN", 936) == CP_UTF8, "Unknown-version test hosts must retain UTF-8 fallback");
        }
        expect(!l2dae::parseHostVersion(nullptr).valid, "Null registration version was dereferenced");
        expect(l2dae::hostTextCodePage(l2dae::parseHostVersion("22"), "ZH-cn", 437) == 936, "Chinese language tag normalization failed");
        expect(l2dae::hostTextCodePage(l2dae::parseHostVersion("22"), "zh_TW", 950) == 950, "Traditional Chinese OEM page was overridden");

        const std::wstring chinese = L"导入动作与表情 / 缩放 (%) / 从当前时间开始";
        const auto utf8 = l2dae::encodeHostText(chinese, CP_UTF8);
        const auto gbk = l2dae::encodeHostText(chinese, 936);
        expect(utf8 != gbk && utf8.size() > gbk.size(), "Legacy and UTF-8 text must use different bytes");
        expect(decode(utf8, CP_UTF8) == chinese && decode(gbk, 936) == chinese, "Chinese host labels must round trip losslessly");
        expect(l2dae::encodeHostUtf8(utf8, 936) == gbk, "UTF-8 translated error was not encoded for old AE");
        expect(l2dae::encodeHostUtf8(utf8, CP_UTF8) == utf8, "Modern AE error was unexpectedly re-encoded");
        reject([&] { l2dae::encodeHostText(chinese, 437); }, "Unsupported legacy label characters were silently replaced");
        reject([&] { l2dae::encodeHostText(L"模型😀", 936); }, "Legacy label emoji must report encoding loss");
        const auto lossy = l2dae::encodeHostText(L"模型😀 error 42", 936, true);
        expect(decode(lossy, 936).find(L"模型") == 0 && lossy.find("error 42") != std::string::npos,
            "Permitted error replacement discarded readable diagnostics");
        reject([&] { l2dae::encodeHostUtf8(std::string("bad\xff", 4), 936); }, "Invalid UTF-8 diagnostics were silently reinterpreted");
        reject([&] { l2dae::encodeHostText(std::wstring(1, wchar_t{0xd800}), CP_UTF8); }, "Unpaired UTF-16 surrogate was encoded as valid UTF-8");
        reject([&] { l2dae::encodeHostText(L"x", 999999); }, "Invalid code page was accepted");
        boundaries(L"A中点B", 936);
        boundaries(L"中A字节边界", 936);
        boundaries(L"A中😀文Z", CP_UTF8);
        boundaries(L"ASCII unchanged", 437);
        std::array<char, 32> field{};
        const auto longLabel = l2dae::encodeHostText(L"动作表情音频呼吸眨眼过渡设置很长的文字", 936);
        const auto copied = l2dae::copyHostText(field.data(), field.size(), longLabel, 936);
        expect(copied == 30 && field[copied] == 0, "31-byte AE field split a GBK character");
        expect(decode(std::string(field.data(), copied), 936).size() == 15, "31-byte AE field did not preserve complete Chinese characters");
        expect(l2dae::copyHostText(nullptr, 32, gbk, 936) == 0, "Null field was dereferenced");
        std::cout << "PASS: " << checks << " host text compatibility checks; AE22-26 version boundary, CP936/UTF-8, loss detection and bounded fields.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
