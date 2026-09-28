// Usage: Live2DImportPreferencesTest <workspace-test-directory>
// All instances receive an explicit private path. The production singleton,
// LOCALAPPDATA path resolver and real user preference file are never invoked.
#include "ImportPreferences.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
std::size_t assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

struct Workspace {
    fs::path parent, directory;
    bool owned = false;
    explicit Workspace(const fs::path& requested) {
        parent = fs::absolute(requested).lexically_normal();
        require(parent != parent.root_path() && !parent.filename().empty(), "Refusing a filesystem root as the test workspace.");
        fs::create_directories(parent);
        directory = parent / (L"import-preferences-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        require(directory.parent_path() == parent && !fs::exists(directory), "Private test directory is not unique.");
        owned = fs::create_directory(directory);
        require(owned, "Cannot create private test directory.");
    }
    ~Workspace() {
        // Remove only the unique directory this test created, never the supplied
        // workspace or an existing caller-owned directory.
        if (owned && directory.is_absolute() && directory.parent_path() == parent &&
            directory.filename().wstring().find(L"import-preferences-") == 0) {
            std::error_code error;
            fs::remove_all(directory, error);
        }
    }
};

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(stream.good(), "Cannot write a private test fixture.");
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "Cannot read private test output.");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string record(int curve) { return "[Import]\r\nTransitionCurve=" + std::to_string(curve) + "\r\n"; }

std::size_t entries(const fs::path& directory) {
    std::size_t result = 0;
    for (const auto& entry : fs::directory_iterator(directory)) { (void)entry; ++result; }
    return result;
}

struct ReadResult {
    std::string text;
    DWORD openError = ERROR_SUCCESS;
    DWORD readError = ERROR_SUCCESS;
};

ReadResult sharedRead(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {{}, GetLastError(), ERROR_SUCCESS};
    char text[128]{};
    DWORD size = 0;
    const bool success = ReadFile(file, text, sizeof text, &size, nullptr) != FALSE;
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return {success ? std::string(text, size) : std::string{}, ERROR_SUCCESS, error};
}

void testDefaultsAndPersistence(const fs::path& root) {
    const auto file = root / L"中文配置_日本語" / L"preferences.ini";
    l2dae::ImportPreferences preferences(file);
    require(preferences.transitionCurve() == 5, "First import must default to clear rebound (5).");
    require(!fs::exists(file.parent_path()), "Constructing or reading created a preference directory.");
    for (int curve : {3, 4, 5, 3, 5, 4}) {
        require(preferences.saveTransitionCurve(curve), "Valid selection did not persist.");
        require(preferences.transitionCurve() == curve, "Current session lost the selection.");
        l2dae::ImportPreferences reopened(file);
        require(reopened.transitionCurve() == curve, "A new instance did not recover the last selection.");
        require(read(file) == record(curve), "Preference file contains unexpected data.");
        require(entries(file.parent_path()) == 1, "Atomic save left temporary files.");
    }
    const auto before = read(file);
    for (int invalid : {(std::numeric_limits<int>::min)(), -1, 0, 1, 2, 6, (std::numeric_limits<int>::max)()}) {
        require(!preferences.saveTransitionCurve(invalid), "Invalid curve was accepted for persistence.");
        require(preferences.transitionCurve() == 4, "Invalid curve changed the session preference.");
        require(read(file) == before, "Invalid curve changed the stored preference.");
    }
    require(entries(file.parent_path()) == 1, "Invalid requests left temporary files.");
    // Loaded state is shared by all model imports using this one app instance.
    write(file, record(3));
    require(preferences.transitionCurve() == 4, "A current session lost its confirmed preference.");
    l2dae::ImportPreferences nextSession(file);
    require(nextSession.transitionCurve() == 3, "A fresh session failed to read the current disk record.");
}

void testCorruption(const fs::path& root) {
    const auto file = root / L"corrupt" / L"preferences.ini";
    const std::vector<std::string> invalid{
        "", " \r\n\t", "4", "[Import]\nTransitionCurve=0\n", "[Import]\nTransitionCurve=2\n",
        "[Import]\nTransitionCurve=6\n", "[Import]\nTransitionCurve=-1\n", "[Import]\nTransitionCurve=4.0\n",
        "[Import]\nTransitionCurve=44\n", "[Import]\nTransitionCurve=4junk\n",
        "[Import]\nTransitionCurve=3\nTransitionCurve=4\n", "[Other]\nTransitionCurve=3\n",
        "[Import]\nTransitionCurve=3\nModelPath=not-stored\n", std::string(129, '5'),
        std::string("[Import]\nTransitionCurve=3\0", 27), std::string("\xef\xbb\xbf") + record(3)
    };
    for (const auto& text : invalid) {
        write(file, text);
        l2dae::ImportPreferences preferences(file);
        require(preferences.transitionCurve() == 5, "Corrupt/unsupported preference did not fall back to 5.");
        require(read(file) == text, "Reading a corrupt preference modified it.");
    }
    write(file, " \t[Import]\nTransitionCurve=3\n\t ");
    l2dae::ImportPreferences alternateLineEndings(file);
    require(alternateLineEndings.transitionCurve() == 3, "Valid LF/outer whitespace was not read.");
    require(alternateLineEndings.saveTransitionCurve(4), "Valid save could not replace a corrupt/old record.");
    require(read(file) == record(4), "Recovered preference was not canonical ASCII.");
    require(entries(file.parent_path()) == 1, "Recovery left temporary files.");
}

void testWriteFailures(const fs::path& root) {
    const auto file = root / L"locked" / L"preferences.ini";
    l2dae::ImportPreferences preferences(file);
    require(preferences.saveTransitionCurve(3), "Cannot seed locked-file test.");
    HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(lock != INVALID_HANDLE_VALUE, "Cannot lock private preference file.");
    l2dae::ImportPreferences lockedReader(file);
    require(lockedReader.transitionCurve() == 5, "A persistently locked preference read did not fall back to 5.");
    const bool saved = preferences.saveTransitionCurve(4);
    CloseHandle(lock);
    require(!saved, "Replacing a locked preference should report a persistence failure.");
    require(preferences.transitionCurve() == 4, "Write failure did not retain the session preference.");
    require(!preferences.saveTransitionCurve(2) && preferences.transitionCurve() == 4,
        "Invalid input replaced the session fallback after a write failure.");
    require(read(file) == record(3), "Failed replacement damaged the previously complete disk record.");
    l2dae::ImportPreferences reopened(file);
    require(reopened.transitionCurve() == 3, "Failed save unexpectedly changed the next-session preference.");
    require(entries(file.parent_path()) == 1, "Failed replacement left a temporary file.");
    require(preferences.saveTransitionCurve(5), "Preference could not recover after releasing a file lock.");
    require(read(file) == record(5), "Recovered save did not commit the new record.");

    const auto blocked = root / L"parent-is-file";
    write(blocked, "unrelated existing content");
    l2dae::ImportPreferences blockedParent(blocked / L"preferences.ini");
    require(blockedParent.transitionCurve() == 5, "Unreadable preference did not use its default.");
    require(!blockedParent.saveTransitionCurve(3) && blockedParent.transitionCurve() == 3,
        "Missing writable directory did not preserve the session choice.");
    require(read(blocked) == "unrelated existing content", "Failed save modified an unrelated blocking file.");

    const auto directoryFile = root / L"destination-is-directory" / L"preferences.ini";
    fs::create_directories(directoryFile);
    l2dae::ImportPreferences directoryDestination(directoryFile);
    require(!directoryDestination.saveTransitionCurve(4) && directoryDestination.transitionCurve() == 4,
        "Directory destination did not preserve a best-effort session choice.");
    require(fs::is_directory(directoryFile) && entries(directoryFile.parent_path()) == 1,
        "Failed directory replacement changed the destination or left a temporary file.");

    l2dae::ImportPreferences noProfile(fs::path{});
    require(noProfile.transitionCurve() == 5, "Unresolved profile must default to 5.");
    require(!noProfile.saveTransitionCurve(3) && noProfile.transitionCurve() == 3,
        "Unresolved profile must keep an in-memory choice without falling back to the current directory.");
}

void testConcurrency(const fs::path& root) {
    const auto file = root / L"concurrent" / L"preferences.ini";
    l2dae::ImportPreferences preferences(file);
    require(preferences.saveTransitionCurve(3), "Cannot seed concurrency test.");
    std::atomic<bool> start{false};
    std::atomic<int> remaining{4}, snapshots{0};
    std::atomic<int> saveFailures{0}, sessionFailures{0}, openFailures{0}, readFailures{0}, malformedRecords{0};
    std::atomic<DWORD> lastSaveError{0}, lastOpenError{0}, lastReadError{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 24; ++i) {
                if (!preferences.saveTransitionCurve(3 + (i + worker) % 3)) {
                    lastSaveError = GetLastError();
                    ++saveFailures;
                }
                const int current = preferences.transitionCurve();
                if (current < 3 || current > 5) ++sessionFailures;
            }
            --remaining;
        });
    }
    std::thread reader([&] {
        while (!start.load()) std::this_thread::yield();
        do {
            const auto snapshot = sharedRead(file);
            if (snapshot.openError) { ++openFailures; lastOpenError = snapshot.openError; }
            else if (snapshot.readError) { ++readFailures; lastReadError = snapshot.readError; }
            else if (snapshot.text != record(3) && snapshot.text != record(4) && snapshot.text != record(5))
                ++malformedRecords;
            ++snapshots;
        } while (remaining.load() != 0);
    });
    start = true;
    for (auto& worker : workers) worker.join();
    reader.join();
    std::cout << "Concurrent diagnostics: saveFailures=" << saveFailures << " lastSaveError=" << lastSaveError
        << " sessionFailures=" << sessionFailures << " openFailures=" << openFailures
        << " lastOpenError=" << lastOpenError << " readFailures=" << readFailures
        << " lastReadError=" << lastReadError << " malformedRecords=" << malformedRecords << '\n';
    require(saveFailures == 0, "A concurrent valid preference save failed; see Win32 diagnostic.");
    require(sessionFailures == 0, "A concurrent session preference escaped 3..5.");
    require(openFailures == 0, "Concurrent preference open failed; see Win32 diagnostic.");
    require(readFailures == 0, "Concurrent preference read failed; see Win32 diagnostic.");
    require(malformedRecords == 0 && snapshots.load() > 0, "Concurrent readers saw an incomplete or malformed record.");
    l2dae::ImportPreferences reopened(file);
    require(reopened.transitionCurve() == preferences.transitionCurve(), "Final disk/session preferences disagree.");
    require(entries(file.parent_path()) == 1, "Concurrent saves left temporary files.");
    std::cout << "Atomic persistence: 96 saves, " << snapshots.load() << " complete concurrent snapshots.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Usage: Live2DImportPreferencesTest <workspace-test-directory>\n";
        return 2;
    }
    try {
        Workspace workspace(argv[1]);
        testDefaultsAndPersistence(workspace.directory);
        testCorruption(workspace.directory);
        testWriteFailures(workspace.directory);
        testConcurrency(workspace.directory);
        std::cout << "PASS: " << assertions << " import preference assertions; default 5, cross-session 3/4/5, corruption, "
            "Unicode paths, write-failure session fallback and atomic concurrency. Real user preferences untouched.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
