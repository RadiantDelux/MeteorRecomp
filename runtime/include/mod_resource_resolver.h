#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeMods {

struct ModStatus {
    std::string id;
    std::string name;
    std::string version;
    std::string platform;
    int32_t priority = 0;
    bool enabled = false;
    bool active = false;
    uint32_t fileOverrideCount = 0;
    uint32_t afsOverrideCount = 0;
    std::string reason;
};

struct FileOverrideRegistration {
    std::string dvdPath;
    std::filesystem::path payloadPath;
    uint32_t payloadSize = 0;
    bool resize = false;
};

enum class FileOverrideReadResult {
    NotHandled,
    Success,
    Failure,
};

// Overlays a successful retail read without changing DVD completion semantics.
void ApplyDvdReadOverlays(std::string_view dvdPath,
                          const std::filesystem::path& hostPath,
                          uint64_t fileOffset,
                          std::vector<uint8_t>& bytes);

std::filesystem::path ModsRootDirectory();
std::filesystem::path ModStatePath();

// Discovers manifests on first use.
std::vector<ModStatus> GetModStatuses();

std::optional<uint32_t> GetResizedFileSize(std::string_view dvdPath);
// Returns a size only if the virtual file extends beyond its retail extent.
std::optional<uint32_t> GetResizedFileSize(std::string_view dvdPath,
                                           const std::filesystem::path& hostPath);
std::vector<FileOverrideRegistration> GetFileOverrideRegistrations();

// Handles ranges beyond the retail file before the host read.
FileOverrideReadResult ReadFileOverride(std::string_view dvdPath,
                                        uint64_t fileOffset,
                                        uint32_t length,
                                        std::vector<uint8_t>& bytes);

// Handles virtual AFS ranges beyond the retail file before the host read.
FileOverrideReadResult ReadVirtualAfs(std::string_view dvdPath,
                                      const std::filesystem::path& hostPath,
                                      uint64_t fileOffset,
                                      uint32_t length,
                                      std::vector<uint8_t>& bytes);

// Changes take effect on the next launch; ModStatus.enabled updates immediately.
bool SetModEnabledForNextLaunch(std::string_view id, bool enabled);
bool SetModPriorityForNextLaunch(std::string_view id, int32_t priority);

} // namespace RuntimeMods
