#pragma once

#include "HostText.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <winver.h>

namespace l2dae {
namespace host_process_detail {

inline bool isSupportedProcessName(std::wstring_view name) noexcept {
    const auto same = [name](std::wstring_view expected) {
        if (name.size() != expected.size()) return false;
        for (size_t i = 0; i < name.size(); ++i) {
            const wchar_t c = name[i] >= L'A' && name[i] <= L'Z' ? name[i] + (L'a' - L'A') : name[i];
            if (c != expected[i]) return false;
        }
        return true;
    };
    return same(L"afterfx.exe") || same(L"aerender.exe");
}

inline HostVersion fixedVersion(const VS_FIXEDFILEINFO* info, size_t bytes) noexcept {
    if (!info || bytes < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE) return {};
    const auto decode = [](DWORD ms, DWORD ls) noexcept {
        HostVersion result;
        result.major = HIWORD(ms); result.minor = LOWORD(ms);
        result.patch = HIWORD(ls); result.build = LOWORD(ls);
        result.valid = result.major != 0;
        return result;
    };
    // The numeric fixed block is independent of translated strings/code pages.
    // ProductVersion describes AE; FileVersion is the fallback for executables
    // that omit their product version. Neither is the effect API's 13.x value.
    const auto product = decode(info->dwProductVersionMS, info->dwProductVersionLS);
    return product.valid ? product : decode(info->dwFileVersionMS, info->dwFileVersionLS);
}

} // namespace host_process_detail

// Read-only path injection for independent tests. Production passes only the
// path of the current host process, never a plug-in path or a folder label.
inline HostVersion readHostExecutableVersion(const std::filesystem::path& executable) noexcept {
    try {
        const auto& path = executable.native();
        if (path.empty() || !executable.is_absolute() || path.size() >= 32768 ||
            path.find(L'\0') != std::wstring::npos) return {};
        DWORD unused = 0;
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
        // A bounded allocation also handles unexpected/corrupt resource sizes.
        if (size < sizeof(VS_FIXEDFILEINFO) || size > 1024u * 1024u) return {};
        std::vector<unsigned char> data(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
        void* fixed = nullptr;
        UINT fixedSize = 0;
        if (!VerQueryValueW(data.data(), L"\\", &fixed, &fixedSize) || !fixed ||
            fixedSize < sizeof(VS_FIXEDFILEINFO)) return {};
        const auto start = reinterpret_cast<std::uintptr_t>(data.data());
        const auto address = reinterpret_cast<std::uintptr_t>(fixed);
        if (address < start || address - start > data.size() - sizeof(VS_FIXEDFILEINFO) ||
            fixedSize > data.size() - (address - start)) return {};
        VS_FIXEDFILEINFO info{};
        std::memcpy(&info, fixed, sizeof info);
        return host_process_detail::fixedVersion(&info, sizeof info);
    } catch (...) {
        return {};
    }
}

inline HostVersion currentProcessHostVersion() noexcept {
    // Cache a single read for this loaded module. PiPL/discovery caching cannot
    // skip this path; it uses the executable containing the running process.
    static const HostVersion version = []() noexcept -> HostVersion {
        try {
            std::vector<wchar_t> path(260);
            for (;;) {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (!length) return {};
                if (length < path.size()) {
                    const std::filesystem::path executable(std::wstring(path.data(), length));
                    if (!host_process_detail::isSupportedProcessName(executable.filename().native())) return {};
                    return readHostExecutableVersion(executable);
                }
                if (path.size() == 32768) return {};
                path.resize((std::min)(path.size() * 2, size_t{32768}));
            }
        } catch (...) {
            return {};
        }
    }();
    return version;
}

} // namespace l2dae
