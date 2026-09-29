#pragma once

#include "platform/host_platform.h"
#include <stdexcept>
#include <string>

#ifndef WIICOMPILED_STORAGE_NAMESPACE
#define WIICOMPILED_STORAGE_NAMESPACE ""
#endif

namespace RuntimeProjectPaths {

inline constexpr std::string_view kStorageNamespace = WIICOMPILED_STORAGE_NAMESPACE;

// One portable directory component, usable unchanged on Windows and Linux.
constexpr bool IsValidNamespace(std::string_view value) {
    if (value.empty()) return true; // Preserve the original layout.
    if (value.size() > 64 || value.front() == '-' || value.front() == '_') return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    if (value == "con" || value == "prn" || value == "aux" || value == "nul") return false;
    if (value.size() == 4 && (value.substr(0, 3) == "com" || value.substr(0, 3) == "lpt") &&
        value[3] >= '1' && value[3] <= '9') return false;
    return true;
}
static_assert(IsValidNamespace(kStorageNamespace), "Invalid WIICOMPILED_STORAGE_NAMESPACE");

inline std::filesystem::path ProjectDirectory(const std::filesystem::path& base,
                                             std::string_view id = kStorageNamespace) {
    if (!IsValidNamespace(id)) throw std::invalid_argument("Invalid storage namespace");
    return id.empty() ? base : base / "Projects" / std::string(id);
}

inline std::optional<std::filesystem::path> FindPortableRoot(
    std::filesystem::path executableDirectory, int maxDepth = 4) {
    for (int level = 0; level <= maxDepth; ++level) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(executableDirectory / "portable.txt", ec)) return executableDirectory;
        const auto parent = executableDirectory.parent_path();
        if (parent.empty() || parent == executableDirectory) break;
        executableDirectory = parent;
    }
    return std::nullopt;
}

inline std::filesystem::path UserDataDirectory(
    const std::optional<std::filesystem::path>& portableRoot,
    std::string_view id = kStorageNamespace) {
    return ProjectDirectory(portableRoot ? *portableRoot / "UserData"
                                        : RuntimePlatform::ApplicationDataDirectory("WiiCompiled"), id);
}

} // namespace RuntimeProjectPaths
