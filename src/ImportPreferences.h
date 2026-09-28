#pragma once

#include <Windows.h>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace l2dae {

// Only the user's last confirmed motion-import rebound preset is persisted.
// Model names, asset paths and project data are deliberately not written here.
// An injected file path keeps tests and callers with a private profile separate
// from the application profile. Constructing/reading never creates directories.
class ImportPreferences {
    struct Handle {
        HANDLE value = INVALID_HANDLE_VALUE;
        explicit Handle(HANDLE handle) : value(handle) {}
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    std::filesystem::path file_;
    std::mutex mutex_;
    int curve_ = 5;
    bool loaded_ = false;

    static bool valid(int curve) noexcept { return curve >= 3 && curve <= 5; }

    int readFile() const noexcept {
        try {
            if (file_.empty()) return 5;
            Handle file(CreateFileW(file_.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.value == INVALID_HANDLE_VALUE) return 5;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > 128) return 5;
            std::array<char, 128> data{};
            DWORD read = 0;
            if (!ReadFile(file.value, data.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) ||
                read != static_cast<DWORD>(size.QuadPart)) return 5;
            std::string text;
            text.reserve(read);
            for (DWORD i = 0; i < read; ++i) {
                const unsigned char c = static_cast<unsigned char>(data[i]);
                if (c == 0 || c > 127) return 5;
                if (c != '\r') text += static_cast<char>(c);
            }
            const auto begin = text.find_first_not_of(" \t\n");
            if (begin == std::string::npos) return 5;
            const auto end = text.find_last_not_of(" \t\n");
            text = text.substr(begin, end - begin + 1);
            const std::string prefix = "[Import]\nTransitionCurve=";
            if (text.size() != prefix.size() + 1 || text.compare(0, prefix.size(), prefix) != 0) return 5;
            const int curve = text.back() - '0';
            return valid(curve) ? curve : 5;
        } catch (...) {
            return 5;
        }
    }

    bool writeFile(int curve) const noexcept {
        try {
            if (file_.empty() || file_.filename().empty() || file_.parent_path().empty()) return false;
            std::error_code error;
            std::filesystem::create_directories(file_.parent_path(), error);
            if (error) return false;
            // The temporary file shares the destination directory/volume. Readers
            // see either the complete old record or the complete new record.
            std::array<wchar_t, MAX_PATH + 1> temporary{};
            if (!GetTempFileNameW(file_.parent_path().c_str(), L"L2D", 0, temporary.data())) return false;
            struct TemporaryFile {
                const wchar_t* path;
                bool committed = false;
                ~TemporaryFile() { if (!committed) DeleteFileW(path); }
            } cleanup{temporary.data()};
            {
                Handle output(CreateFileW(temporary.data(), GENERIC_WRITE, 0, nullptr,
                    TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
                if (output.value == INVALID_HANDLE_VALUE) return false;
                const std::string text = "[Import]\r\nTransitionCurve=" + std::to_string(curve) + "\r\n";
                DWORD written = 0;
                if (!WriteFile(output.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
                    written != text.size() || !FlushFileBuffers(output.value)) return false;
            }
            // A reader can temporarily prevent replacement even when it uses
            // FILE_SHARE_DELETE. Retry only file-sharing/access conflicts with
            // a short bounded wait; a persistent lock remains a non-fatal save
            // failure. Keep the old destination intact throughout every retry.
            for (unsigned attempt = 0; ; ++attempt) {
                if (MoveFileExW(temporary.data(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    break;
                const DWORD replaceError = GetLastError();
                if ((replaceError != ERROR_ACCESS_DENIED && replaceError != ERROR_SHARING_VIOLATION &&
                    replaceError != ERROR_LOCK_VIOLATION) || attempt == 31) return false;
                Sleep(1);
            }
            cleanup.committed = true;
            return true;
        } catch (...) {
            return false;
        }
    }

public:
    explicit ImportPreferences(std::filesystem::path storageFile) : file_(std::move(storageFile)) {}

    int transitionCurve() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!loaded_) { curve_ = readFile(); loaded_ = true; }
            return curve_;
        } catch (...) {
            return 5;
        }
    }

    // Invoke only after the motion import has been successfully submitted.
    // Persistence failure is non-fatal: the confirmed choice still applies in
    // this process, while the previous complete disk record remains available.
    bool saveTransitionCurve(int curve) noexcept {
        if (!valid(curve)) return false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            curve_ = curve;
            loaded_ = true;
            return writeFile(curve);
        } catch (...) {
            return false;
        }
    }
};

inline std::filesystem::path defaultImportPreferencesPath() {
    // Read-only path resolution. If the profile location cannot be resolved,
    // the instance keeps an in-memory preference and never writes to the CWD.
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!required || required > 32768) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
    if (!written || written >= required) return {};
    value.resize(written);
    const std::filesystem::path directory(value);
    if (!directory.is_absolute()) return {};
    return directory / L"Live2DNativeAE" / L"preferences.ini";
}

inline ImportPreferences& applicationImportPreferences() {
    static ImportPreferences preferences(defaultImportPreferencesPath());
    return preferences;
}

inline int lastImportTransitionCurve() noexcept {
    try { return applicationImportPreferences().transitionCurve(); }
    catch (...) { return 5; }
}

inline bool rememberImportTransitionCurve(int curve) noexcept {
    try { return applicationImportPreferences().saveTransitionCurve(curve); }
    catch (...) { return false; }
}

} // namespace l2dae
