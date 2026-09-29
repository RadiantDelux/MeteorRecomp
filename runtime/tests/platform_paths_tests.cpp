#include "platform/host_platform.h"

#include <iostream>
#include <cstdlib>

int main() {
    if (!RuntimePlatform::ExecutableDirectory()) {
        std::cerr << "unable to resolve the current executable directory\n";
        return 1;
    }

    // Launching from a different working directory must not relocate sidecars.
    const auto originalCwd = std::filesystem::current_path();
    const auto executableDirectory = RuntimePlatform::ExecutableDirectory();
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    const auto movedDirectory = RuntimePlatform::ExecutableDirectory();
    std::filesystem::current_path(originalCwd);
    if (movedDirectory != executableDirectory) {
        std::cerr << "executable directory depends on working directory\n";
        return 1;
    }
#if defined(__linux__)
    const auto checkData = [](const std::filesystem::path& expected) {
        return RuntimePlatform::ApplicationDataDirectory("PathContract") == expected;
    };
    setenv("HOME", "/tmp/wiicompiled home", 1);
    setenv("XDG_DATA_HOME", "/tmp/wiicompiled data", 1);
    if (!checkData("/tmp/wiicompiled data/PathContract")) return 1;
    setenv("XDG_DATA_HOME", "relative/path", 1);
    if (!checkData("/tmp/wiicompiled home/.local/share/PathContract")) return 1;
    unsetenv("XDG_DATA_HOME");
    if (!checkData("/tmp/wiicompiled home/.local/share/PathContract")) return 1;
#endif

    const auto userData = RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest");
    if (userData.filename() != "WiiCompiledPlatformPathsTest") {
        std::cerr << "application-data directory lost its application name: " << userData << '\n';
        return 1;
    }
    if (RuntimePlatform::LogDirectory("WiiCompiledPlatformPathsTest") != userData / "Logs") {
        std::cerr << "log directory is not derived from application data\n";
        return 1;
    }
#if defined(__APPLE__)
    if (userData.parent_path().filename() != "Application Support" ||
        userData.parent_path().parent_path().filename() != "Library") {
        std::cerr << "macOS application-data directory is not under ~/Library/Application Support: "
                  << userData << '\n';
        return 1;
    }
#endif

    return 0;
}
