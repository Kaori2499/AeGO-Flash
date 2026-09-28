// Usage: Live2DHostProcessTest [AfterFX.exe [expected-major expected-minor]]
// All file accesses are read-only. No AE process is launched or controlled.
#include "HostProcess.h"
#include <iostream>
#include <stdexcept>

namespace {
size_t checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool equals(l2dae::HostVersion version, unsigned major, unsigned minor, unsigned patch, unsigned build) {
    return version.valid && version.major == major && version.minor == minor &&
        version.patch == patch && version.build == build;
}
void testFixedVersion() {
    VS_FIXEDFILEINFO info{};
    info.dwSignature = VS_FFI_SIGNATURE;
    info.dwStrucVersion = 0x10000;
    info.dwProductVersionMS = MAKELONG(3, 26);
    info.dwProductVersionLS = MAKELONG(87, 1);
    info.dwFileVersionMS = MAKELONG(4, 25);
    info.dwFileVersionLS = MAKELONG(10, 2);
    require(equals(l2dae::host_process_detail::fixedVersion(&info, sizeof info), 26, 3, 1, 87),
        "Numeric product version must take priority over a different file version.");
    info.dwProductVersionMS = 0;
    require(equals(l2dae::host_process_detail::fixedVersion(&info, sizeof info), 25, 4, 2, 10),
        "Absent product major must fall back to the numeric file version.");
    info.dwProductVersionMS = MAKELONG(7, 0);
    require(equals(l2dae::host_process_detail::fixedVersion(&info, sizeof info), 25, 4, 2, 10),
        "A product version with major zero is invalid even when its other fields are set.");
    info.dwFileVersionMS = 0;
    require(!l2dae::host_process_detail::fixedVersion(&info, sizeof info).valid,
        "Missing product and file versions cannot identify a host.");
    info.dwProductVersionMS = static_cast<DWORD>(MAKELONG(65535, 65535));
    info.dwProductVersionLS = static_cast<DWORD>(MAKELONG(65535, 65535));
    require(equals(l2dae::host_process_detail::fixedVersion(&info, sizeof info), 65535, 65535, 65535, 65535),
        "All fixed-version words must remain unsigned and retain their full range.");
    require(!l2dae::host_process_detail::fixedVersion(nullptr, sizeof info).valid, "Null fixed block must be rejected.");
    for (size_t bytes : {size_t{0}, sizeof info - 1})
        require(!l2dae::host_process_detail::fixedVersion(&info, bytes).valid, "Truncated fixed block must be rejected.");
    info.dwSignature = 0;
    require(!l2dae::host_process_detail::fixedVersion(&info, sizeof info).valid, "Wrong fixed-block signature must be rejected.");
}
void testProcessBoundary() {
    for (const auto* accepted : {L"AfterFX.exe", L"afterfx.exe", L"AFTERFX.EXE", L"aerender.exe", L"AeRender.ExE"})
        require(l2dae::host_process_detail::isSupportedProcessName(accepted), "Supported host basename was rejected.");
    for (const auto* rejected : {L"", L"AfterFX", L"AfterFX.exe.old", L"SomeAfterFX.exe", L"AeGOFlash.aex",
        L"Live2DAdapterSmoke.exe", L"Adobe After Effects 2025", L"C:\\Adobe\\AfterFX.exe"})
        require(!l2dae::host_process_detail::isSupportedProcessName(rejected), "Non-host basename was accepted.");
    const auto first = l2dae::currentProcessHostVersion();
    require(!first.valid, "This independent test process must not be mistaken for AE.");
    for (int i = 0; i < 10; ++i) {
        const auto repeated = l2dae::currentProcessHostVersion();
        require(!repeated.valid && repeated.major == first.major && repeated.minor == first.minor,
            "Repeated cached lookup changed the non-host result.");
    }
    require(!l2dae::readHostExecutableVersion({}).valid, "Empty executable path must be rejected.");
    require(!l2dae::readHostExecutableVersion(L"AfterFX.exe").valid, "Relative paths must not search the current directory.");
    std::wstring embeddedNul = L"C:\\invalid";
    embeddedNul.push_back(L'\0'); embeddedNul += L"AfterFX.exe";
    require(!l2dae::readHostExecutableVersion(std::filesystem::path(embeddedNul)).valid,
        "An embedded NUL must not silently shorten the requested path.");
    require(!l2dae::readHostExecutableVersion(std::filesystem::path(L"C:\\" + std::wstring(32768, L'a'))).valid,
        "Oversized paths must fail without an unbounded allocation.");
    const auto missing = std::filesystem::current_path() / L"不存在的宿主_日本語" / L"AfterFX.exe";
    require(!l2dae::readHostExecutableVersion(missing).valid, "Missing Unicode executable must fail safely.");
    require(!l2dae::readHostExecutableVersion(std::filesystem::current_path()).valid, "A directory is not an executable.");
    // This source file is a present non-PE fixture; the resource reader must not
    // infer a version from strings, project paths or the directory's name.
    const auto source = std::filesystem::absolute(std::filesystem::path(__FILE__));
    if (std::filesystem::is_regular_file(source))
        require(!l2dae::readHostExecutableVersion(source).valid, "A source file must not supply a host version.");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 1 || argc == 2 || argc == 4, "Usage: Live2DHostProcessTest [AfterFX.exe [expected-major expected-minor]]");
        testFixedVersion();
        testProcessBoundary();
        if (argc >= 2) {
            const auto executable = std::filesystem::absolute(argv[1]);
            const auto version = l2dae::readHostExecutableVersion(executable);
            require(version.valid, "The supplied actual host executable has no valid numeric version.");
            if (argc == 4)
                require(version.major == std::stoul(argv[2]) && version.minor == std::stoul(argv[3]),
                    "Actual host VERSIONINFO disagrees with the independently supplied expected version.");
            std::cout << "Actual executable product/file version: " << version.major << '.' << version.minor << '.'
                << version.patch << '.' << version.build << '\n';
        }
        std::cout << "PASS: " << checks << " runtime host-version checks; numeric product/file fallback, invalid resources, "
            "exact process-name boundary and cached non-host rejection. All file access was read-only.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
