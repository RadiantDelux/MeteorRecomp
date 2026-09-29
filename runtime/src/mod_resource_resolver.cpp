#include "mod_resource_resolver.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace RuntimeMods {
namespace {

struct AfsOverride {
    std::string modId;
    std::string modName;
    std::string platform;
    int32_t priority = 0;
    std::string dvdPath;
    uint32_t entryIndex = 0;
    fs::path payloadPath;
    uint64_t payloadSize = 0;
    std::vector<std::string> dependencies;
    bool resize = false;
};

struct FileOverride {
    std::string modId;
    std::string modName;
    std::string platform;
    int32_t priority = 0;
    std::string dvdPath;
    fs::path payloadPath;
    uint64_t payloadSize = 0;
    std::vector<std::string> dependencies;
    bool explicitMapping = false;
    bool resize = false;
};

struct ModMetadata {
    std::string id;
    int32_t priority = 0;
    std::vector<std::string> dependencies;
    std::vector<std::string> conflicts;
};

struct ModUserState {
    std::optional<bool> enabled;
    std::optional<int32_t> priority;
};

struct AfsLayout {
    uint64_t byteOffset = 0;
    uint32_t byteSize = 0;
};

struct AfsTraceEntry {
    uint32_t index = 0;
    uint64_t byteOffset = 0;
    uint32_t byteSize = 0;
};

struct VirtualAfsEntry {
    uint32_t index = 0;
    uint64_t virtualOffset = 0;
    uint32_t virtualSize = 0;
    uint64_t retailOffset = 0;
    uint32_t retailSize = 0;
    const AfsOverride* override = nullptr;
};

struct VirtualAfsPlan {
    bool valid = false;
    uint64_t retailFileSize = 0;
    uint64_t dataStart = 0;
    uint64_t dataEnd = 0;
    std::vector<uint8_t> header;
    std::vector<VirtualAfsEntry> entries;
};

struct ResolverState {
    std::vector<ModStatus> modStatuses;
    std::map<std::string, ModUserState> userStates;
    std::vector<FileOverride> fileOverrides;
    std::vector<AfsOverride> afsOverrides;
    std::set<std::string> rejectedFileOverrides;
    std::map<std::pair<std::string, uint32_t>, AfsLayout> layoutCache;
    std::set<std::pair<std::string, uint32_t>> rejectedLayouts;
    std::set<std::pair<std::string, uint32_t>> loggedHits;
    std::map<std::string, std::vector<AfsTraceEntry>> traceLayouts;
    std::set<std::pair<std::string, uint32_t>> tracedEntries;
    std::map<std::string, VirtualAfsPlan> virtualAfsPlans;
    std::set<std::string> rejectedVirtualAfsPlans;
    bool traceLimitLogged = false;
    std::mutex mutex;
};

static ResolverState g_state;
static std::once_flag g_loadOnce;

static uint64_t CurrentProcessIdForTempFile() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

static std::optional<fs::path> StateTempPathFor(const fs::path& target) {
    const std::string pid = std::to_string(CurrentProcessIdForTempFile());
    for (uint32_t attempt = 0; attempt < 64; ++attempt) {
        fs::path candidate = target;
        candidate += ".tmp." + pid;
        if (attempt != 0) {
            candidate += "." + std::to_string(attempt);
        }
        std::error_code ec;
        const bool exists = fs::exists(candidate, ec);
        if (ec) {
            return std::nullopt;
        }
        if (!exists) {
            return candidate;
        }
    }
    return std::nullopt;
}

static bool PublishStateFileAtomically(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code ec;
    fs::rename(temporary, target, ec);
    return !ec;
#endif
}

static uint32_t ReadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

static void WriteLe32(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static std::string AsciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static std::string NormalizeDvdPath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    std::vector<std::string> components;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() && value[cursor] == '/') {
            ++cursor;
        }
        const size_t start = cursor;
        while (cursor < value.size() && value[cursor] != '/') {
            ++cursor;
        }
        if (start == cursor) {
            continue;
        }
        std::string component = AsciiLower(value.substr(start, cursor - start));
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            if (!components.empty()) {
                components.pop_back();
            }
            continue;
        }
        components.push_back(std::move(component));
    }

    std::string normalized = "/";
    for (size_t i = 0; i < components.size(); ++i) {
        if (i != 0) {
            normalized.push_back('/');
        }
        normalized += components[i];
    }
    return normalized;
}

static bool IsValidModId(std::string_view id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (const unsigned char ch : id) {
        if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')) {
            return false;
        }
    }
    return true;
}

template <typename T>
static std::optional<T> FindValue(const toml::value& value, const char* key) {
    try {
        if (!value.contains(key)) {
            return std::nullopt;
        }
        return toml::find<T>(value, key);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

static std::optional<uint32_t> FindUint32(const toml::value& value, const char* key) {
    const auto integer = FindValue<int64_t>(value, key);
    if (!integer || *integer < 0 || static_cast<uint64_t>(*integer) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*integer);
}

static std::optional<int32_t> FindInt32(const toml::value& value, const char* key) {
    const auto integer = FindValue<int64_t>(value, key);
    if (!integer || *integer < INT32_MIN || *integer > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*integer);
}

static std::optional<uint64_t> PayloadFileSize(const fs::path& path) {
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > UINT32_MAX) {
        return std::nullopt;
    }
    return size;
}

static bool PackageRelativePathIsSafe(const fs::path& packageRoot,
                                      const fs::path& relative,
                                      bool requireDirectory) {
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }

    fs::path current = packageRoot;
    for (const fs::path& component : relative) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        current /= component;
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec || fs::is_symlink(status)) {
            return false;
        }
    }

    std::error_code ec;
    return requireDirectory ? fs::is_directory(current, ec) && !ec
                            : fs::is_regular_file(current, ec) && !ec;
}

static std::optional<fs::path> ResolvePackagePayload(const fs::path& manifestPath,
                                                     const std::string& value) {
    fs::path relative = RuntimeConfigFile::PathFromUtf8(value);
    if (relative.empty() || relative.is_absolute()) {
        return std::nullopt;
    }
    relative = relative.lexically_normal();
    for (const fs::path& component : relative) {
        if (component == "..") {
            return std::nullopt;
        }
    }
    const fs::path resolved = (manifestPath.parent_path() / relative).lexically_normal();
    if (!PackageRelativePathIsSafe(manifestPath.parent_path(), relative, false)) {
        return std::nullopt;
    }
    return resolved;
}

static bool ReadPayloadRange(const fs::path& path,
                             uint64_t payloadSize,
                             uint64_t payloadOffset,
                             uint8_t* destination,
                             size_t length) {
    if (length == 0) {
        return true;
    }
    if (payloadOffset > payloadSize || static_cast<uint64_t>(length) > payloadSize - payloadOffset) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);
    stream.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(length));
    return stream && static_cast<size_t>(stream.gcount()) == length;
}

static std::optional<AfsLayout> ReadAfsLayout(const fs::path& hostPath, uint32_t index) {
    std::ifstream stream(hostPath, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    uint8_t header[8] = {};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!stream || header[0] != 'A' || header[1] != 'F' || header[2] != 'S' || header[3] != 0) {
        return std::nullopt;
    }
    const uint32_t count = ReadLe32(header + 4);
    if (index >= count) {
        return std::nullopt;
    }

    const uint64_t tableOffset = 8ull + static_cast<uint64_t>(index) * 8ull;
    if (tableOffset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }
    stream.seekg(static_cast<std::streamoff>(tableOffset), std::ios::beg);
    uint8_t entry[8] = {};
    stream.read(reinterpret_cast<char*>(entry), sizeof(entry));
    if (!stream) {
        return std::nullopt;
    }

    const uint32_t offset = ReadLe32(entry + 0);
    const uint32_t size = ReadLe32(entry + 4);
    if (size == 0) {
        return std::nullopt;
    }
    return AfsLayout{offset, size};
}

static std::optional<std::vector<AfsTraceEntry>> ReadAfsTraceLayout(const fs::path& hostPath) {
    std::ifstream stream(hostPath, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    stream.seekg(0, std::ios::end);
    const std::streamoff fileSizeSigned = stream.tellg();
    if (fileSizeSigned <= 0) {
        return std::nullopt;
    }
    const uint64_t fileSize = static_cast<uint64_t>(fileSizeSigned);
    stream.seekg(0, std::ios::beg);

    uint8_t header[8] = {};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!stream || header[0] != 'A' || header[1] != 'F' || header[2] != 'S' || header[3] != 0) {
        return std::nullopt;
    }
    const uint32_t count = ReadLe32(header + 4);
    const uint64_t tableBytes = static_cast<uint64_t>(count) * 8ull;
    if (count == 0 || tableBytes > fileSize - 8ull || tableBytes > SIZE_MAX) {
        return std::nullopt;
    }

    std::vector<uint8_t> table(static_cast<size_t>(tableBytes));
    stream.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
    if (!stream || static_cast<size_t>(stream.gcount()) != table.size()) {
        return std::nullopt;
    }

    std::vector<AfsTraceEntry> entries;
    entries.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t* entry = table.data() + static_cast<size_t>(index) * 8u;
        const uint32_t offset = ReadLe32(entry + 0);
        const uint32_t size = ReadLe32(entry + 4);
        if (size == 0 || offset > fileSize || static_cast<uint64_t>(size) > fileSize - offset) {
            continue;
        }
        entries.push_back(AfsTraceEntry{index, offset, size});
    }
    std::sort(entries.begin(), entries.end(), [](const AfsTraceEntry& a, const AfsTraceEntry& b) {
        if (a.byteOffset != b.byteOffset) {
            return a.byteOffset < b.byteOffset;
        }
        return a.index < b.index;
    });
    return entries;
}

static const std::optional<std::string>& TraceAfsPath() {
    static const std::optional<std::string> value = []() -> std::optional<std::string> {
        const char* text = std::getenv("METEOR_TRACE_MOD_AFS_READS");
        if (!text || !text[0]) {
            return std::nullopt;
        }
        if (std::string_view(text) == "*") {
            return std::string("*");
        }
        return NormalizeDvdPath(text);
    }();
    return value;
}

static void TraceAfsRead(const std::string& normalizedPath,
                         const fs::path& hostPath,
                         uint64_t fileOffset,
                         size_t byteCount) {
    const auto& configuredPath = TraceAfsPath();
    if (!configuredPath || (*configuredPath != "*" && *configuredPath != normalizedPath) || byteCount == 0) {
        return;
    }
    if (*configuredPath == "*" &&
        (normalizedPath.size() < 4 || normalizedPath.substr(normalizedPath.size() - 4) != ".afs")) {
        return;
    }

    static constexpr size_t kTraceEntryLimit = 512;
    const uint64_t readStart = fileOffset;
    const uint64_t readEnd = readStart + static_cast<uint64_t>(byteCount);

    std::lock_guard<std::mutex> lock(g_state.mutex);
    auto cached = g_state.traceLayouts.find(normalizedPath);
    if (cached == g_state.traceLayouts.end()) {
        const auto parsed = ReadAfsTraceLayout(hostPath);
        if (!parsed) {
            RT_LOGF(RT_TAG_DVD, "mod AFS trace disabled for %s: archive table could not be parsed\n",
                    normalizedPath.c_str());
            g_state.traceLayouts.emplace(normalizedPath, std::vector<AfsTraceEntry>{});
            return;
        }
        cached = g_state.traceLayouts.emplace(normalizedPath, std::move(*parsed)).first;
        RT_LOGF(RT_TAG_DVD, "mod AFS trace armed: %s entries=%zu\n",
                normalizedPath.c_str(), cached->second.size());
    }

    const auto& entries = cached->second;
    if (entries.empty()) {
        return;
    }
    auto it = std::lower_bound(entries.begin(), entries.end(), readStart,
                               [](const AfsTraceEntry& entry, uint64_t value) {
                                   return entry.byteOffset < value;
                               });
    if (it != entries.begin()) {
        --it;
    }
    for (; it != entries.end() && it->byteOffset < readEnd; ++it) {
        const uint64_t entryEnd = it->byteOffset + it->byteSize;
        if (entryEnd <= readStart) {
            continue;
        }
        const auto key = std::make_pair(normalizedPath, it->index);
        if (g_state.tracedEntries.count(key) != 0) {
            continue;
        }
        if (g_state.tracedEntries.size() >= kTraceEntryLimit) {
            if (!g_state.traceLimitLogged) {
                g_state.traceLimitLogged = true;
                RT_LOGF(RT_TAG_DVD, "mod AFS trace reached %zu unique-entry limit\n", kTraceEntryLimit);
            }
            return;
        }
        g_state.tracedEntries.insert(key);
        RT_LOGF(RT_TAG_DVD,
                "mod AFS trace hit: %s[%u] entry=0x%llx+%u read=0x%llx+%zu\n",
                normalizedPath.c_str(), it->index,
                static_cast<unsigned long long>(it->byteOffset), it->byteSize,
                static_cast<unsigned long long>(readStart), byteCount);
    }
}

static std::optional<VirtualAfsPlan> BuildVirtualAfsPlan(const std::string& normalizedPath,
                                                        const fs::path& hostPath) {
    bool needsResize = false;
    for (const AfsOverride& override : g_state.afsOverrides) {
        if (override.dvdPath == normalizedPath && override.resize) {
            needsResize = true;
            break;
        }
    }
    if (!needsResize) {
        return std::nullopt;
    }

    std::error_code ec;
    const uint64_t fileSize = fs::file_size(hostPath, ec);
    if (ec || fileSize < 16) {
        return VirtualAfsPlan{};
    }

    std::ifstream stream(hostPath, std::ios::binary);
    if (!stream) {
        return VirtualAfsPlan{};
    }
    uint8_t header8[8] = {};
    stream.read(reinterpret_cast<char*>(header8), sizeof(header8));
    if (!stream || header8[0] != 'A' || header8[1] != 'F' || header8[2] != 'S' || header8[3] != 0) {
        return VirtualAfsPlan{};
    }
    const uint32_t count = ReadLe32(header8 + 4);
    const uint64_t tableBytes = static_cast<uint64_t>(count) * 8ull;
    if (count == 0 || tableBytes > fileSize - 8ull || tableBytes > SIZE_MAX) {
        return VirtualAfsPlan{};
    }

    std::vector<uint8_t> table(static_cast<size_t>(tableBytes));
    stream.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
    if (!stream || static_cast<size_t>(stream.gcount()) != table.size()) {
        return VirtualAfsPlan{};
    }

    std::vector<AfsLayout> retailEntries(count);
    uint64_t firstDataOffset = fileSize;
    uint64_t previousOffset = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t* entry = table.data() + static_cast<size_t>(index) * 8u;
        const uint32_t offset = ReadLe32(entry + 0);
        const uint32_t size = ReadLe32(entry + 4);
        retailEntries[index] = AfsLayout{offset, size};
        if (size == 0) {
            continue;
        }
        if ((offset & 0x7FFu) != 0 || offset < previousOffset ||
            offset > fileSize || static_cast<uint64_t>(size) > fileSize - offset) {
            return VirtualAfsPlan{};
        }
        previousOffset = offset;
        firstDataOffset = std::min<uint64_t>(firstDataOffset, offset);
    }
    const uint64_t minimumHeader = 8ull + tableBytes;
    if (firstDataOffset == fileSize || firstDataOffset < minimumHeader ||
        firstDataOffset > SIZE_MAX) {
        return VirtualAfsPlan{};
    }

    VirtualAfsPlan plan;
    plan.retailFileSize = fileSize;
    plan.dataStart = firstDataOffset;
    plan.header.resize(static_cast<size_t>(firstDataOffset));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(plan.header.data()),
                static_cast<std::streamsize>(plan.header.size()));
    if (!stream || static_cast<size_t>(stream.gcount()) != plan.header.size()) {
        return VirtualAfsPlan{};
    }

    std::vector<const AfsOverride*> byIndex(count, nullptr);
    for (const AfsOverride& override : g_state.afsOverrides) {
        if (override.dvdPath != normalizedPath) {
            continue;
        }
        if (override.entryIndex >= count) {
            return VirtualAfsPlan{};
        }
        byIndex[override.entryIndex] = &override;
    }

    uint64_t cursor = firstDataOffset;
    plan.entries.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const AfsLayout retail = retailEntries[index];
        if (retail.byteSize == 0) {
            continue;
        }
        cursor = AlignUp(cursor, 0x800ull);
        const AfsOverride* override = byIndex[index];
        const uint64_t desiredSize = override && override->resize
                                         ? override->payloadSize
                                         : static_cast<uint64_t>(retail.byteSize);
        if (desiredSize == 0 || desiredSize > UINT32_MAX || cursor > UINT32_MAX) {
            return VirtualAfsPlan{};
        }
        const uint64_t end = cursor + desiredSize;
        // AFS offsets and sizes are u32 byte fields. The virtual archive may
        // grow past the extracted retail EOF now that the runtime FST can give
        // it a synthetic extent, but it must remain representable by the AFS
        // table itself.
        if (end < cursor || end > UINT32_MAX) {
            return VirtualAfsPlan{};
        }

        plan.entries.push_back(VirtualAfsEntry{
            index,
            cursor,
            static_cast<uint32_t>(desiredSize),
            retail.byteOffset,
            retail.byteSize,
            override,
        });
        uint8_t* tableEntry = plan.header.data() + 8u + static_cast<size_t>(index) * 8u;
        WriteLe32(tableEntry + 0, static_cast<uint32_t>(cursor));
        WriteLe32(tableEntry + 4, static_cast<uint32_t>(desiredSize));
        cursor = end;
    }
    plan.dataEnd = cursor;
    plan.valid = true;
    return plan;
}

static const VirtualAfsPlan* GetOrBuildVirtualAfsPlan(const std::string& normalizedPath,
                                                      const fs::path& hostPath,
                                                      bool& rejected) {
    rejected = false;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.rejectedVirtualAfsPlans.count(normalizedPath) != 0) {
        rejected = true;
        return nullptr;
    }
    if (const auto found = g_state.virtualAfsPlans.find(normalizedPath);
        found != g_state.virtualAfsPlans.end()) {
        return &found->second;
    }

    const auto built = BuildVirtualAfsPlan(normalizedPath, hostPath);
    if (!built.has_value()) {
        return nullptr;
    }
    if (!built->valid) {
        g_state.rejectedVirtualAfsPlans.insert(normalizedPath);
        rejected = true;
        RT_LOGF(RT_TAG_DVD,
                "mod virtual AFS rejected: %s cannot build a safe u32 layout\n",
                normalizedPath.c_str());
        return nullptr;
    }

    const auto inserted = g_state.virtualAfsPlans.emplace(normalizedPath, std::move(*built));
    const VirtualAfsPlan* plan = &inserted.first->second;
    RT_LOGF(RT_TAG_DVD,
            "mod virtual AFS armed: %s entries=%zu data=0x%llx..0x%llx retailSize=%llu publishedSize=%llu\n",
            normalizedPath.c_str(), plan->entries.size(),
            static_cast<unsigned long long>(plan->dataStart),
            static_cast<unsigned long long>(plan->dataEnd),
            static_cast<unsigned long long>(plan->retailFileSize),
            static_cast<unsigned long long>(std::max(plan->retailFileSize, plan->dataEnd)));
    return plan;
}

static bool ApplyVirtualAfsPlan(const VirtualAfsPlan& plan,
                                const fs::path& hostPath,
                                uint64_t fileOffset,
                                std::vector<uint8_t>& bytes) {
    if (!plan.valid || bytes.empty()) {
        return false;
    }

    const uint64_t readStart = fileOffset;
    const uint64_t readEnd = readStart + bytes.size();
    std::vector<uint8_t> staged = bytes;

    const uint64_t headerEnd = plan.header.size();
    const uint64_t headerOverlapStart = std::min(readStart, headerEnd);
    const uint64_t headerOverlapEnd = std::min(readEnd, headerEnd);
    if (headerOverlapStart < headerOverlapEnd) {
        const size_t destination = static_cast<size_t>(headerOverlapStart - readStart);
        const size_t source = static_cast<size_t>(headerOverlapStart);
        const size_t count = static_cast<size_t>(headerOverlapEnd - headerOverlapStart);
        std::copy_n(plan.header.begin() + source, count, staged.begin() + destination);
    }

    const uint64_t dataOverlapStart = std::max(readStart, plan.dataStart);
    const uint64_t dataOverlapEnd = std::min(readEnd, plan.dataEnd);
    if (dataOverlapStart < dataOverlapEnd) {
        std::fill(staged.begin() + static_cast<size_t>(dataOverlapStart - readStart),
                  staged.begin() + static_cast<size_t>(dataOverlapEnd - readStart), 0);
    }

    std::optional<std::ifstream> retail;
    auto it = std::lower_bound(plan.entries.begin(), plan.entries.end(), readStart,
                               [](const VirtualAfsEntry& entry, uint64_t value) {
                                   return entry.virtualOffset < value;
                               });
    if (it != plan.entries.begin()) {
        --it;
    }
    for (; it != plan.entries.end() && it->virtualOffset < readEnd; ++it) {
        const VirtualAfsEntry& entry = *it;
        const uint64_t entryStart = entry.virtualOffset;
        const uint64_t entryEnd = entryStart + entry.virtualSize;
        const uint64_t overlapStart = std::max(readStart, entryStart);
        const uint64_t overlapEnd = std::min(readEnd, entryEnd);
        if (overlapStart >= overlapEnd) {
            continue;
        }
        const size_t destination = static_cast<size_t>(overlapStart - readStart);
        const size_t withinEntry = static_cast<size_t>(overlapStart - entryStart);
        const size_t count = static_cast<size_t>(overlapEnd - overlapStart);

        if (entry.override) {
            const size_t available = withinEntry < entry.override->payloadSize
                                         ? static_cast<size_t>(entry.override->payloadSize - withinEntry)
                                         : 0;
            const size_t copyCount = std::min(count, available);
            if (copyCount != 0) {
                if (!ReadPayloadRange(entry.override->payloadPath,
                                      entry.override->payloadSize,
                                      withinEntry,
                                      staged.data() + destination,
                                      copyCount)) {
                    return false;
                }
            }
            continue;
        }

        if (withinEntry > entry.retailSize || count > entry.retailSize - withinEntry) {
            return false;
        }
        if (!retail.has_value()) {
            retail.emplace(hostPath, std::ios::binary);
            if (!*retail) {
                return false;
            }
        }
        const uint64_t sourceOffset = entry.retailOffset + withinEntry;
        retail->seekg(static_cast<std::streamoff>(sourceOffset), std::ios::beg);
        retail->read(reinterpret_cast<char*>(staged.data() + destination),
                     static_cast<std::streamsize>(count));
        if (!*retail || static_cast<size_t>(retail->gcount()) != count) {
            return false;
        }
    }

    bytes.swap(staged);
    return true;
}

static std::vector<fs::path> DiscoverManifests(const fs::path& root) {
    std::vector<fs::path> manifests;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        return manifests;
    }

    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_symlink(ec) || ec) {
            ec.clear();
            continue;
        }
        if (!it->is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        const fs::path manifest = it->path() / "mod.toml";
        if (!fs::is_symlink(manifest, ec) && !ec && fs::is_regular_file(manifest, ec) && !ec) {
            manifests.push_back(manifest);
        }
        ec.clear();
    }
    std::sort(manifests.begin(), manifests.end(), [](const fs::path& a, const fs::path& b) {
        return RuntimeConfigFile::PathToUtf8(a) < RuntimeConfigFile::PathToUtf8(b);
    });
    return manifests;
}

static std::string TomlQuote(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(ch); break;
        }
    }
    result.push_back('"');
    return result;
}

static void LoadUserStates() {
    const fs::path path = ModStatePath();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }

    toml::value document;
    try {
        document = toml::parse(input, RuntimeConfigFile::PathToUtf8(path));
    } catch (const std::exception& exception) {
        RT_LOGF(RT_TAG_DVD, "mod state ignored: %s: %s\n",
                RuntimeConfigFile::PathToUtf8(path).c_str(), exception.what());
        return;
    }
    if (!document.contains("mods") || !document.at("mods").is_array()) {
        return;
    }

    for (const toml::value& item : document.at("mods").as_array()) {
        if (!item.is_table()) {
            continue;
        }
        const auto id = FindValue<std::string>(item, "id");
        if (!id || !IsValidModId(*id)) {
            continue;
        }
        ModUserState state;
        state.enabled = FindValue<bool>(item, "enabled");
        state.priority = FindInt32(item, "priority");
        if (state.enabled || state.priority) {
            g_state.userStates[*id] = state;
        }
    }
}

static void LoadImplicitWiiFolder(const fs::path& packageRoot,
                                  std::set<std::string>& seenIds,
                                  std::set<std::string>& enabledIds,
                                  std::vector<ModMetadata>& enabledMods,
                                  std::vector<FileOverride>& discoveredFiles) {
    const std::string id = RuntimeConfigFile::PathToUtf8(packageRoot.filename());
    if (!IsValidModId(id)) {
        RT_LOGF(RT_TAG_DVD, "implicit Wii mod ignored: invalid folder id %s\n", id.c_str());
        return;
    }
    if (!seenIds.insert(id).second) {
        RT_LOGF(RT_TAG_DVD, "implicit Wii mod ignored: duplicate id %s\n", id.c_str());
        return;
    }

    bool enabled = false;
    int32_t priority = 0;
    if (const auto state = g_state.userStates.find(id); state != g_state.userStates.end()) {
        if (state->second.enabled) {
            enabled = *state->second.enabled;
        }
        if (state->second.priority) {
            priority = *state->second.priority;
        }
    }

    const size_t statusIndex = g_state.modStatuses.size();
    g_state.modStatuses.push_back(ModStatus{
        id,
        id,
        "",
        "wii",
        priority,
        enabled,
        enabled,
        0,
        0,
        enabled ? std::string{} : std::string{"disabled"},
    });
    if (!enabled) {
        RT_LOGF(RT_TAG_DVD, "implicit Wii mod discovered: %s disabled priority=%d\n",
                id.c_str(), priority);
        return;
    }

    enabledIds.insert(id);
    enabledMods.push_back(ModMetadata{id, priority, {}, {}});
    const fs::path fileRoot = packageRoot / "files";
    std::error_code ec;
    size_t accepted = 0;
    for (fs::recursive_directory_iterator it(
             fileRoot, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end;
         it.increment(ec)) {
        std::error_code itemEc;
        if (it->is_symlink(itemEc) || itemEc || !it->is_regular_file(itemEc) || itemEc) {
            itemEc.clear();
            continue;
        }
        const fs::path relative = fs::relative(it->path(), fileRoot, itemEc);
        if (itemEc || relative.empty()) {
            continue;
        }
        const auto payloadSize = PayloadFileSize(it->path());
        if (!payloadSize) {
            continue;
        }
        FileOverride override;
        override.modId = id;
        override.modName = id;
        override.platform = "wii";
        override.priority = priority;
        override.dvdPath = NormalizeDvdPath("/" + relative.generic_string());
        override.payloadPath = it->path().lexically_normal();
        override.payloadSize = *payloadSize;
        override.explicitMapping = false;
        override.resize = true;
        discoveredFiles.push_back(std::move(override));
        ++accepted;
    }
    g_state.modStatuses[statusIndex].fileOverrideCount = static_cast<uint32_t>(accepted);
    RT_LOGF(RT_TAG_DVD, "implicit Wii mod loaded: %s priority=%d file overrides=%zu\n",
            id.c_str(), priority, accepted);
}

static bool SaveUserStatesLocked() {
    const fs::path path = ModStatePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    const auto temporary = StateTempPathFor(path);
    if (!temporary) {
        return false;
    }

    {
        std::ofstream output(*temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output << "# Meteor Mod Framework user overrides. Generated by the runtime.\n";
        for (const auto& [id, state] : g_state.userStates) {
            if (!state.enabled && !state.priority) {
                continue;
            }
            output << "\n[[mods]]\n";
            output << "id = " << TomlQuote(id) << "\n";
            if (state.enabled) {
                output << "enabled = " << (*state.enabled ? "true" : "false") << "\n";
            }
            if (state.priority) {
                output << "priority = " << *state.priority << "\n";
            }
        }
        output.flush();
        output.close();
        if (!output) {
            fs::remove(*temporary, ec);
            return false;
        }
    }

    if (!PublishStateFileAtomically(*temporary, path)) {
        fs::remove(*temporary, ec);
        RT_LOGF(RT_TAG_DVD, "mod state save failed: could not atomically publish %s\n",
                RuntimeConfigFile::PathToUtf8(path).c_str());
        return false;
    }
    return true;
}

static void LoadManifest(const fs::path& manifestPath,
                         std::set<std::string>& seenIds,
                         std::set<std::string>& enabledIds,
                         std::vector<ModMetadata>& enabledMods,
                         std::vector<FileOverride>& discoveredFiles,
                         std::vector<AfsOverride>& discovered) {
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input) {
        return;
    }

    toml::value document;
    try {
        document = toml::parse(input, RuntimeConfigFile::PathToUtf8(manifestPath));
    } catch (const std::exception& exception) {
        RT_LOGF(RT_TAG_DVD, "mod manifest ignored: %s: %s\n",
                RuntimeConfigFile::PathToUtf8(manifestPath).c_str(), exception.what());
        return;
    }

    if (!document.contains("mod")) {
        RT_LOGF(RT_TAG_DVD, "mod manifest ignored: %s has no [mod] table\n",
                RuntimeConfigFile::PathToUtf8(manifestPath).c_str());
        return;
    }
    const toml::value& mod = document.at("mod");
    const auto id = FindValue<std::string>(mod, "id");
    if (!id || !IsValidModId(*id)) {
        RT_LOGF(RT_TAG_DVD, "mod manifest ignored: %s has invalid mod.id\n",
                RuntimeConfigFile::PathToUtf8(manifestPath).c_str());
        return;
    }
    if (!seenIds.insert(*id).second) {
        RT_LOGF(RT_TAG_DVD, "duplicate mod id ignored: %s (%s)\n", id->c_str(),
                RuntimeConfigFile::PathToUtf8(manifestPath).c_str());
        return;
    }

    const std::string name = FindValue<std::string>(mod, "name").value_or(*id);
    const std::string version = FindValue<std::string>(mod, "version").value_or("");
    const std::string platform = AsciiLower(FindValue<std::string>(mod, "platform").value_or("wii"));
    bool enabled = FindValue<bool>(mod, "enabled").value_or(false);
    int32_t priority = FindInt32(mod, "priority").value_or(0);
    if (const auto state = g_state.userStates.find(*id); state != g_state.userStates.end()) {
        if (state->second.enabled) {
            enabled = *state->second.enabled;
        }
        if (state->second.priority) {
            priority = *state->second.priority;
        }
    }
    const size_t statusIndex = g_state.modStatuses.size();
    g_state.modStatuses.push_back(ModStatus{
        *id,
        name,
        version,
        platform,
        priority,
        enabled,
        false,
        0,
        0,
        enabled ? std::string{} : std::string{"disabled"},
    });
    if (!enabled) {
        RT_LOGF(RT_TAG_DVD, "mod discovered: %s (%s) disabled priority=%d\n",
                id->c_str(), platform.c_str(), priority);
        return;
    }
    if (platform != "wii" && platform != "ps2" && platform != "meteor") {
        RT_LOGF(RT_TAG_DVD, "mod ignored: %s has unsupported platform '%s'\n",
                id->c_str(), platform.c_str());
        g_state.modStatuses[statusIndex].reason = "unsupported platform";
        return;
    }
    g_state.modStatuses[statusIndex].active = true;
    std::vector<std::string> dependencies =
        FindValue<std::vector<std::string>>(mod, "requires").value_or(std::vector<std::string>{});
    dependencies.erase(std::remove_if(dependencies.begin(), dependencies.end(), [](const std::string& value) {
        return !IsValidModId(value);
    }), dependencies.end());
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    std::vector<std::string> conflicts =
        FindValue<std::vector<std::string>>(mod, "conflicts").value_or(std::vector<std::string>{});
    conflicts.erase(std::remove_if(conflicts.begin(), conflicts.end(), [&](const std::string& value) {
        return !IsValidModId(value) || value == *id;
    }), conflicts.end());
    std::sort(conflicts.begin(), conflicts.end());
    conflicts.erase(std::unique(conflicts.begin(), conflicts.end()), conflicts.end());
    enabledIds.insert(*id);
    enabledMods.push_back(ModMetadata{*id, priority, dependencies, conflicts});

    size_t acceptedFiles = 0;
    const bool fileRootResize = FindValue<bool>(mod, "file_root_resize").value_or(false);
    if (const auto fileRootText = FindValue<std::string>(mod, "file_root");
        fileRootText && !fileRootText->empty()) {
        fs::path relativeRoot = RuntimeConfigFile::PathFromUtf8(*fileRootText);
        if (relativeRoot.is_absolute()) {
            RT_LOGF(RT_TAG_DVD, "mod file_root ignored: %s must be relative to mod.toml\n", id->c_str());
        } else {
            relativeRoot = relativeRoot.lexically_normal();
            bool escapes = relativeRoot.empty();
            for (const fs::path& component : relativeRoot) {
                if (component == "..") {
                    escapes = true;
                    break;
                }
            }
            const fs::path fileRoot = manifestPath.parent_path() / relativeRoot;
            std::error_code ec;
            if (!escapes &&
                PackageRelativePathIsSafe(manifestPath.parent_path(), relativeRoot, true)) {
                for (fs::recursive_directory_iterator it(
                         fileRoot, fs::directory_options::skip_permission_denied, ec),
                     end;
                     !ec && it != end;
                     it.increment(ec)) {
                    std::error_code itemEc;
                    if (it->is_symlink(itemEc) || itemEc || !it->is_regular_file(itemEc) || itemEc) {
                        itemEc.clear();
                        continue;
                    }
                    const fs::path relative = fs::relative(it->path(), fileRoot, itemEc);
                    if (itemEc || relative.empty()) {
                        continue;
                    }
                    const auto payloadSize = PayloadFileSize(it->path());
                    if (!payloadSize) {
                        continue;
                    }
                    FileOverride override;
                    override.modId = *id;
                    override.modName = name;
                    override.platform = platform;
                    override.priority = priority;
                    override.dvdPath = NormalizeDvdPath("/" + relative.generic_string());
                    override.payloadPath = it->path().lexically_normal();
                    override.payloadSize = *payloadSize;
                    override.dependencies = dependencies;
                    override.explicitMapping = false;
                    override.resize = fileRootResize;
                    discoveredFiles.push_back(std::move(override));
                    ++acceptedFiles;
                }
            } else if (!escapes) {
                RT_LOGF(RT_TAG_DVD, "mod file_root not found: %s -> %s\n",
                        id->c_str(), RuntimeConfigFile::PathToUtf8(fileRoot).c_str());
            } else {
                RT_LOGF(RT_TAG_DVD, "mod file_root ignored: %s escapes its package directory\n", id->c_str());
            }
        }
    }
    if (document.contains("file_overrides")) {
        const toml::value& filesValue = document.at("file_overrides");
        if (!filesValue.is_array()) {
            RT_LOGF(RT_TAG_DVD, "mod ignored: %s file_overrides is not an array\n", id->c_str());
            g_state.modStatuses[statusIndex].active = false;
            g_state.modStatuses[statusIndex].reason = "invalid file_overrides";
            return;
        }
        for (const toml::value& item : filesValue.as_array()) {
            if (!item.is_table()) {
                continue;
            }
            const auto path = FindValue<std::string>(item, "path");
            const auto payload = FindValue<std::string>(item, "payload");
            const bool resize = FindValue<bool>(item, "resize").value_or(false);
            if (!path || !payload || payload->empty()) {
                continue;
            }

            const auto resolvedPayload = ResolvePackagePayload(manifestPath, *payload);
            if (!resolvedPayload) {
                RT_LOGF(RT_TAG_DVD,
                        "mod file override ignored: %s payload escapes/is unavailable in package: %s\n",
                        id->c_str(), payload->c_str());
                continue;
            }
            fs::path payloadPath = *resolvedPayload;

            const auto payloadSize = PayloadFileSize(payloadPath);
            if (!payloadSize) {
                RT_LOGF(RT_TAG_DVD, "mod file override ignored: %s payload unavailable: %s\n",
                        id->c_str(), RuntimeConfigFile::PathToUtf8(payloadPath).c_str());
                continue;
            }

            FileOverride override;
            override.modId = *id;
            override.modName = name;
            override.platform = platform;
            override.priority = priority;
            override.dvdPath = NormalizeDvdPath(*path);
            override.payloadPath = std::move(payloadPath);
            override.payloadSize = *payloadSize;
            override.dependencies = dependencies;
            override.explicitMapping = true;
            override.resize = resize;
            discoveredFiles.push_back(std::move(override));
            ++acceptedFiles;
        }
    }

    if (!document.contains("afs_overrides")) {
        g_state.modStatuses[statusIndex].fileOverrideCount = static_cast<uint32_t>(acceptedFiles);
        RT_LOGF(RT_TAG_DVD, "mod loaded: %s (%s) priority=%d file overrides=%zu AFS overrides=0\n",
                id->c_str(), platform.c_str(), priority, acceptedFiles);
        return;
    }

    const toml::value& overridesValue = document.at("afs_overrides");
    if (!overridesValue.is_array()) {
        RT_LOGF(RT_TAG_DVD, "mod ignored: %s afs_overrides is not an array\n", id->c_str());
        g_state.modStatuses[statusIndex].active = false;
        g_state.modStatuses[statusIndex].reason = "invalid afs_overrides";
        return;
    }

    size_t accepted = 0;
    for (const toml::value& item : overridesValue.as_array()) {
        if (!item.is_table()) {
            continue;
        }
        const auto archive = FindValue<std::string>(item, "archive");
        const auto index = FindUint32(item, "index");
        const auto payload = FindValue<std::string>(item, "payload");
        if (!archive || !index || !payload || payload->empty()) {
            continue;
        }

        const auto resolvedPayload = ResolvePackagePayload(manifestPath, *payload);
        if (!resolvedPayload) {
            RT_LOGF(RT_TAG_DVD,
                    "mod override ignored: %s payload escapes/is unavailable in package: %s\n",
                    id->c_str(), payload->c_str());
            continue;
        }
        fs::path payloadPath = *resolvedPayload;

        const auto payloadSize = PayloadFileSize(payloadPath);
        if (!payloadSize) {
            RT_LOGF(RT_TAG_DVD, "mod override ignored: %s payload unavailable: %s\n",
                    id->c_str(), RuntimeConfigFile::PathToUtf8(payloadPath).c_str());
            continue;
        }

        AfsOverride override;
        override.modId = *id;
        override.modName = name;
        override.platform = platform;
        override.priority = priority;
        override.dvdPath = NormalizeDvdPath(*archive);
        override.entryIndex = *index;
        override.payloadPath = std::move(payloadPath);
        override.payloadSize = *payloadSize;
        override.dependencies = dependencies;
        override.resize = FindValue<bool>(item, "resize").value_or(false);
        discovered.push_back(std::move(override));
        ++accepted;
    }

    RT_LOGF(RT_TAG_DVD,
            "mod loaded: %s (%s) priority=%d file overrides=%zu AFS overrides=%zu\n",
            id->c_str(), platform.c_str(), priority, acceptedFiles, accepted);
    g_state.modStatuses[statusIndex].fileOverrideCount = static_cast<uint32_t>(acceptedFiles);
    g_state.modStatuses[statusIndex].afsOverrideCount = static_cast<uint32_t>(accepted);
}

static void LoadMods() {
    const fs::path root = ModsRootDirectory();
    LoadUserStates();
    const auto manifests = DiscoverManifests(root);

    std::set<std::string> seenIds;
    std::set<std::string> enabledIds;
    std::vector<ModMetadata> enabledMods;
    std::vector<FileOverride> discoveredFiles;
    std::vector<AfsOverride> discovered;
    for (const fs::path& manifest : manifests) {
        LoadManifest(manifest, seenIds, enabledIds, enabledMods, discoveredFiles, discovered);
    }

    std::error_code implicitEc;
    if (fs::is_directory(root, implicitEc) && !implicitEc) {
        for (fs::directory_iterator it(root, implicitEc), end;
             !implicitEc && it != end;
             it.increment(implicitEc)) {
            std::error_code entryEc;
            if (it->is_symlink(entryEc) || entryEc) {
                entryEc.clear();
                continue;
            }
            if (!it->is_directory(entryEc) || entryEc) {
                entryEc.clear();
                continue;
            }
            const fs::path packageRoot = it->path();
            if (fs::is_regular_file(packageRoot / "mod.toml", entryEc) && !entryEc) {
                continue;
            }
            entryEc.clear();
            const fs::path implicitFiles = packageRoot / "files";
            if (fs::is_symlink(implicitFiles, entryEc) || entryEc ||
                !fs::is_directory(implicitFiles, entryEc) || entryEc) {
                entryEc.clear();
                continue;
            }
            LoadImplicitWiiFolder(packageRoot, seenIds, enabledIds, enabledMods, discoveredFiles);
        }
    }

    std::sort(enabledMods.begin(), enabledMods.end(), [](const ModMetadata& a, const ModMetadata& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.id < b.id;
    });

    auto findStatus = [&](const std::string& id) -> ModStatus* {
        const auto found = std::find_if(g_state.modStatuses.begin(), g_state.modStatuses.end(),
                                        [&](const ModStatus& status) { return status.id == id; });
        return found == g_state.modStatuses.end() ? nullptr : &*found;
    };

    // Dependency failures can invalidate a high-priority conflict winner. In
    // that case lower-priority mods it had blocked must be allowed to compete
    // again. Recompute conflicts from scratch whenever a currently-active mod
    // loses a dependency. Dependency rejections are monotonic, so this fixpoint
    // always terminates even for pathological dependency/conflict cycles.
    std::set<std::string> dependencyRejected;
    std::map<std::string, std::string> dependencyFailure;

    bool missingDependencyChanged = true;
    while (missingDependencyChanged) {
        missingDependencyChanged = false;
        for (const ModMetadata& mod : enabledMods) {
            if (dependencyRejected.count(mod.id) != 0) {
                continue;
            }
            for (const std::string& dependency : mod.dependencies) {
                if (enabledIds.count(dependency) == 0 ||
                    dependencyRejected.count(dependency) != 0) {
                    dependencyRejected.insert(mod.id);
                    dependencyFailure.emplace(mod.id, dependency);
                    missingDependencyChanged = true;
                    break;
                }
            }
        }
    }

    std::set<std::string> activeIds;
    std::map<std::string, std::string> conflictWinners;
    for (;;) {
        activeIds.clear();
        conflictWinners.clear();
        std::map<std::string, std::string> blockedBy;

        for (const ModMetadata& mod : enabledMods) {
            if (dependencyRejected.count(mod.id) != 0) {
                continue;
            }

            std::string winner;
            if (const auto blocked = blockedBy.find(mod.id); blocked != blockedBy.end()) {
                winner = blocked->second;
            } else {
                for (const std::string& conflict : mod.conflicts) {
                    if (activeIds.count(conflict) != 0) {
                        winner = conflict;
                        break;
                    }
                }
            }
            if (!winner.empty()) {
                conflictWinners.emplace(mod.id, std::move(winner));
                continue;
            }

            activeIds.insert(mod.id);
            for (const std::string& conflict : mod.conflicts) {
                blockedBy.emplace(conflict, mod.id);
            }
        }

        bool rejectedActiveDependency = false;
        for (const ModMetadata& mod : enabledMods) {
            if (activeIds.count(mod.id) == 0) {
                continue;
            }
            for (const std::string& dependency : mod.dependencies) {
                if (activeIds.count(dependency) == 0) {
                    if (dependencyRejected.insert(mod.id).second) {
                        dependencyFailure.emplace(mod.id, dependency);
                        rejectedActiveDependency = true;
                    }
                    break;
                }
            }
        }
        if (!rejectedActiveDependency) {
            break;
        }
    }

    for (const ModMetadata& mod : enabledMods) {
        ModStatus* status = findStatus(mod.id);
        if (dependencyRejected.count(mod.id) != 0) {
            const auto failure = dependencyFailure.find(mod.id);
            const std::string dependency =
                failure != dependencyFailure.end() ? failure->second : std::string{"unknown"};
            RT_LOGF(RT_TAG_DVD,
                    "mod dependency missing: %s requires active mod %s; mod disabled\n",
                    mod.id.c_str(), dependency.c_str());
            if (status) {
                status->active = false;
                status->reason = "requires " + dependency;
            }
            continue;
        }

        if (activeIds.count(mod.id) != 0) {
            if (status) {
                status->active = true;
                status->reason.clear();
            }
            continue;
        }

        const auto winner = conflictWinners.find(mod.id);
        if (winner != conflictWinners.end()) {
            RT_LOGF(RT_TAG_DVD,
                    "mod conflict: %s disabled because it conflicts with higher-priority mod %s\n",
                    mod.id.c_str(), winner->second.c_str());
            if (status) {
                status->active = false;
                status->reason = "conflicts with " + winner->second;
            }
        }
    }

    discoveredFiles.erase(
        std::remove_if(discoveredFiles.begin(), discoveredFiles.end(), [&](const FileOverride& candidate) {
            return activeIds.count(candidate.modId) == 0;
        }),
        discoveredFiles.end());

    discovered.erase(
        std::remove_if(discovered.begin(), discovered.end(), [&](const AfsOverride& candidate) {
            return activeIds.count(candidate.modId) == 0;
        }),
        discovered.end());

    std::sort(discovered.begin(), discovered.end(), [](const AfsOverride& a, const AfsOverride& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        if (a.modId != b.modId) {
            return a.modId < b.modId;
        }
        if (a.dvdPath != b.dvdPath) {
            return a.dvdPath < b.dvdPath;
        }
        return a.entryIndex < b.entryIndex;
    });

    std::sort(discoveredFiles.begin(), discoveredFiles.end(), [](const FileOverride& a, const FileOverride& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        if (a.modId != b.modId) {
            return a.modId < b.modId;
        }
        if (a.dvdPath != b.dvdPath) {
            return a.dvdPath < b.dvdPath;
        }
        return a.explicitMapping && !b.explicitMapping;
    });

    std::set<std::string> claimedFiles;
    std::map<std::string, std::string> claimedFileOwners;
    for (FileOverride& candidate : discoveredFiles) {
        if (!claimedFiles.insert(candidate.dvdPath).second) {
            const auto owner = claimedFileOwners.find(candidate.dvdPath);
            if (owner != claimedFileOwners.end() && owner->second == candidate.modId) {
                RT_LOGF(RT_TAG_DVD,
                        "mod file override shadowed inside %s: %s (explicit mapping wins file_root)\n",
                        candidate.modId.c_str(), candidate.dvdPath.c_str());
            } else {
                RT_LOGF(RT_TAG_DVD,
                        "mod conflict: lower-priority file override ignored: %s -> %s\n",
                        candidate.modId.c_str(), candidate.dvdPath.c_str());
            }
            continue;
        }
        claimedFileOwners[candidate.dvdPath] = candidate.modId;
        g_state.fileOverrides.push_back(std::move(candidate));
    }

    std::set<std::pair<std::string, uint32_t>> claimed;
    for (AfsOverride& candidate : discovered) {
        const auto key = std::make_pair(candidate.dvdPath, candidate.entryIndex);
        if (!claimed.insert(key).second) {
            RT_LOGF(RT_TAG_DVD,
                    "mod conflict: lower-priority override ignored: %s -> %s[%u]\n",
                    candidate.modId.c_str(), candidate.dvdPath.c_str(), candidate.entryIndex);
            continue;
        }
        g_state.afsOverrides.push_back(std::move(candidate));
    }

    auto outranks = [](int32_t priorityA, const std::string& idA,
                       int32_t priorityB, const std::string& idB) {
        if (priorityA != priorityB) {
            return priorityA > priorityB;
        }
        return idA < idB;
    };

    std::set<std::string> removeFilePaths;
    std::set<std::string> removeAfsPaths;
    for (const FileOverride& file : g_state.fileOverrides) {
        const AfsOverride* bestAfs = nullptr;
        for (const AfsOverride& afs : g_state.afsOverrides) {
            if (afs.dvdPath != file.dvdPath) {
                continue;
            }
            if (!bestAfs || outranks(afs.priority, afs.modId, bestAfs->priority, bestAfs->modId)) {
                bestAfs = &afs;
            }
        }
        if (!bestAfs) {
            continue;
        }
        if (outranks(file.priority, file.modId, bestAfs->priority, bestAfs->modId)) {
            removeAfsPaths.insert(file.dvdPath);
            RT_LOGF(RT_TAG_DVD,
                    "mod provider conflict: file override %s wins %s over AFS-entry providers\n",
                    file.modId.c_str(), file.dvdPath.c_str());
        } else {
            removeFilePaths.insert(file.dvdPath);
            RT_LOGF(RT_TAG_DVD,
                    "mod provider conflict: AFS override %s wins %s over whole-file provider %s\n",
                    bestAfs->modId.c_str(), file.dvdPath.c_str(), file.modId.c_str());
        }
    }

    if (!removeFilePaths.empty()) {
        g_state.fileOverrides.erase(
            std::remove_if(g_state.fileOverrides.begin(), g_state.fileOverrides.end(),
                           [&](const FileOverride& candidate) {
                               return removeFilePaths.count(candidate.dvdPath) != 0;
                           }),
            g_state.fileOverrides.end());
    }
    if (!removeAfsPaths.empty()) {
        g_state.afsOverrides.erase(
            std::remove_if(g_state.afsOverrides.begin(), g_state.afsOverrides.end(),
                           [&](const AfsOverride& candidate) {
                               return removeAfsPaths.count(candidate.dvdPath) != 0;
                           }),
            g_state.afsOverrides.end());
    }

    if (!g_state.fileOverrides.empty() || !g_state.afsOverrides.empty()) {
        RT_LOGF(RT_TAG_DVD,
                "Meteor Mod Framework: %zu active file override(s), %zu active AFS override(s) from %zu active mod(s) (%zu package(s) discovered)\n",
                g_state.fileOverrides.size(), g_state.afsOverrides.size(), activeIds.size(),
                g_state.modStatuses.size());
    }
}

static void EnsureLoaded() {
    std::call_once(g_loadOnce, LoadMods);
}

static const FileOverride* FindFileOverride(std::string_view dvdPath) {
    const std::string normalizedPath = NormalizeDvdPath(std::string(dvdPath));
    const auto found = std::find_if(g_state.fileOverrides.begin(), g_state.fileOverrides.end(),
                                    [&](const FileOverride& value) {
                                        return value.dvdPath == normalizedPath;
                                    });
    return found == g_state.fileOverrides.end() ? nullptr : &*found;
}

} // namespace

fs::path ModsRootDirectory() {
    if (const char* overrideRoot = std::getenv("METEOR_MOD_ROOT")) {
        if (overrideRoot[0] != '\0') {
            return RuntimeConfigFile::PathFromUtf8(overrideRoot).lexically_normal();
        }
    }
    return RuntimeConfigFile::ApplicationDataDirectory() / "Mods";
}

fs::path ModStatePath() {
    return ModsRootDirectory() / "state.toml";
}

std::vector<ModStatus> GetModStatuses() {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.modStatuses;
}

std::optional<uint32_t> GetResizedFileSize(std::string_view dvdPath) {
    EnsureLoaded();
    const FileOverride* override = FindFileOverride(dvdPath);
    if (!override || !override->resize || override->payloadSize > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(override->payloadSize);
}

std::optional<uint32_t> GetResizedFileSize(std::string_view dvdPath,
                                           const fs::path& hostPath) {
    if (const auto wholeFile = GetResizedFileSize(dvdPath)) {
        return wholeFile;
    }

    EnsureLoaded();
    const std::string normalizedPath = NormalizeDvdPath(std::string(dvdPath));
    bool rejected = false;
    const VirtualAfsPlan* plan = GetOrBuildVirtualAfsPlan(normalizedPath, hostPath, rejected);
    if (!plan || rejected) {
        return std::nullopt;
    }
    const uint64_t publishedSize = std::max(plan->retailFileSize, plan->dataEnd);
    if (publishedSize <= plan->retailFileSize || publishedSize > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(publishedSize);
}

std::vector<FileOverrideRegistration> GetFileOverrideRegistrations() {
    EnsureLoaded();
    std::vector<FileOverrideRegistration> result;
    result.reserve(g_state.fileOverrides.size());
    for (const FileOverride& override : g_state.fileOverrides) {
        if (override.payloadSize > UINT32_MAX) {
            continue;
        }
        result.push_back(FileOverrideRegistration{
            override.dvdPath,
            override.payloadPath,
            static_cast<uint32_t>(override.payloadSize),
            override.resize,
        });
    }
    return result;
}

FileOverrideReadResult ReadFileOverride(std::string_view dvdPath,
                                        uint64_t fileOffset,
                                        uint32_t length,
                                        std::vector<uint8_t>& bytes) {
    EnsureLoaded();
    const FileOverride* override = FindFileOverride(dvdPath);
    if (!override) {
        return FileOverrideReadResult::NotHandled;
    }
    if (fileOffset > UINT64_MAX - length) {
        return FileOverrideReadResult::Failure;
    }
    if (override->resize && fileOffset + length > override->payloadSize) {
        return FileOverrideReadResult::Failure;
    }

    bytes.assign(length, 0);
    if (length != 0 && fileOffset < override->payloadSize) {
        const size_t copyCount = static_cast<size_t>(std::min<uint64_t>(
            length, override->payloadSize - fileOffset));
        if (!ReadPayloadRange(override->payloadPath,
                              override->payloadSize,
                              fileOffset,
                              bytes.data(),
                              copyCount)) {
            return FileOverrideReadResult::Failure;
        }
    }

    const auto key = std::make_pair(override->dvdPath, UINT32_MAX);
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.loggedHits.insert(key).second) {
        RT_LOGF(RT_TAG_DVD,
                "mod file override hit: %s (%s) -> %s payload=%zu resize=%s read=0x%llx+%u\n",
                override->modId.c_str(), override->platform.c_str(), override->dvdPath.c_str(),
                static_cast<size_t>(override->payloadSize), override->resize ? "true" : "false",
                static_cast<unsigned long long>(fileOffset), length);
    }
    return FileOverrideReadResult::Success;
}

FileOverrideReadResult ReadVirtualAfs(std::string_view dvdPath,
                                      const fs::path& hostPath,
                                      uint64_t fileOffset,
                                      uint32_t length,
                                      std::vector<uint8_t>& bytes) {
    EnsureLoaded();
    const std::string normalizedPath = NormalizeDvdPath(std::string(dvdPath));
    bool rejected = false;
    const VirtualAfsPlan* plan = GetOrBuildVirtualAfsPlan(normalizedPath, hostPath, rejected);
    if (!plan || rejected) {
        return FileOverrideReadResult::NotHandled;
    }
    if (fileOffset > UINT64_MAX - length) {
        return FileOverrideReadResult::Failure;
    }
    const uint64_t publishedSize = std::max(plan->retailFileSize, plan->dataEnd);
    const uint64_t readEnd = fileOffset + length;
    if (readEnd > publishedSize) {
        return FileOverrideReadResult::Failure;
    }

    bytes.assign(length, 0);

    // The virtualized AFS data range is reconstructed by ApplyVirtualAfsPlan.
    // Preserve any retail trailer after dataEnd (some AFS files may carry
    // metadata/slack there) without requiring a successful host read for the
    // virtual range that can now extend beyond retail EOF.
    const uint64_t retailTailStart = std::max(fileOffset, plan->dataEnd);
    const uint64_t retailTailEnd = std::min(readEnd, plan->retailFileSize);
    if (retailTailStart < retailTailEnd) {
        std::ifstream retail(hostPath, std::ios::binary);
        if (!retail) {
            return FileOverrideReadResult::Failure;
        }
        const size_t destination = static_cast<size_t>(retailTailStart - fileOffset);
        const size_t count = static_cast<size_t>(retailTailEnd - retailTailStart);
        retail.seekg(static_cast<std::streamoff>(retailTailStart), std::ios::beg);
        retail.read(reinterpret_cast<char*>(bytes.data() + destination),
                    static_cast<std::streamsize>(count));
        if (!retail || static_cast<size_t>(retail.gcount()) != count) {
            return FileOverrideReadResult::Failure;
        }
    }

    if (length != 0 && !ApplyVirtualAfsPlan(*plan, hostPath, fileOffset, bytes)) {
        return FileOverrideReadResult::Failure;
    }

    const auto key = std::make_pair(normalizedPath, UINT32_MAX - 1u);
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.loggedHits.insert(key).second) {
        RT_LOGF(RT_TAG_DVD,
                "mod virtual AFS direct read hit: %s published=%llu read=0x%llx+%u\n",
                normalizedPath.c_str(), static_cast<unsigned long long>(publishedSize),
                static_cast<unsigned long long>(fileOffset), length);
    }
    return FileOverrideReadResult::Success;
}

bool SetModEnabledForNextLaunch(std::string_view id, bool enabled) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const auto status = std::find_if(g_state.modStatuses.begin(), g_state.modStatuses.end(),
                                     [&](const ModStatus& value) { return value.id == id; });
    if (status == g_state.modStatuses.end()) {
        return false;
    }
    const auto previous = g_state.userStates.find(status->id);
    const std::optional<ModUserState> previousState =
        previous != g_state.userStates.end() ? std::optional<ModUserState>{previous->second} : std::nullopt;
    g_state.userStates[status->id].enabled = enabled;
    if (!SaveUserStatesLocked()) {
        if (previousState) {
            g_state.userStates[status->id] = *previousState;
        } else {
            g_state.userStates.erase(status->id);
        }
        return false;
    }
    status->enabled = enabled;
    if (status->active != enabled) {
        status->reason = "restart required";
    }
    return true;
}

bool SetModPriorityForNextLaunch(std::string_view id, int32_t priority) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const auto status = std::find_if(g_state.modStatuses.begin(), g_state.modStatuses.end(),
                                     [&](const ModStatus& value) { return value.id == id; });
    if (status == g_state.modStatuses.end()) {
        return false;
    }
    const auto previous = g_state.userStates.find(status->id);
    const std::optional<ModUserState> previousState =
        previous != g_state.userStates.end() ? std::optional<ModUserState>{previous->second} : std::nullopt;
    g_state.userStates[status->id].priority = priority;
    if (!SaveUserStatesLocked()) {
        if (previousState) {
            g_state.userStates[status->id] = *previousState;
        } else {
            g_state.userStates.erase(status->id);
        }
        return false;
    }
    status->priority = priority;
    status->reason = "restart required";
    return true;
}

void ApplyDvdReadOverlays(std::string_view dvdPath,
                          const fs::path& hostPath,
                          uint64_t fileOffset,
                          std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return;
    }
    EnsureLoaded();
    const std::string normalizedPath = NormalizeDvdPath(std::string(dvdPath));
    TraceAfsRead(normalizedPath, hostPath, fileOffset, bytes.size());
    for (const FileOverride& override : g_state.fileOverrides) {
        if (override.dvdPath != normalizedPath) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            if (g_state.rejectedFileOverrides.count(normalizedPath) != 0) {
                return;
            }
            std::error_code ec;
            const uint64_t retailSize = fs::file_size(hostPath, ec);
            if (ec || (!override.resize && override.payloadSize > retailSize)) {
                g_state.rejectedFileOverrides.insert(normalizedPath);
                if (ec) {
                    RT_LOGF(RT_TAG_DVD,
                            "mod file override rejected: %s cannot stat retail file %s\n",
                            override.modId.c_str(), normalizedPath.c_str());
                } else {
                    RT_LOGF(RT_TAG_DVD,
                            "mod file override rejected: %s payload=%zu exceeds retail %s size=%llu\n",
                            override.modId.c_str(), static_cast<size_t>(override.payloadSize), normalizedPath.c_str(),
                            static_cast<unsigned long long>(retailSize));
                }
                return;
            }
        }
        const uint64_t readStart = fileOffset;
        const uint64_t readEnd = readStart + bytes.size();
        if (readStart >= override.payloadSize) {
            std::fill(bytes.begin(), bytes.end(), 0);
        } else {
            const size_t available = static_cast<size_t>(override.payloadSize - readStart);
            const size_t copyCount = std::min(bytes.size(), available);
            if (copyCount != 0 &&
                !ReadPayloadRange(override.payloadPath,
                                  override.payloadSize,
                                  readStart,
                                  bytes.data(),
                                  copyCount)) {
                RT_LOGF(RT_TAG_DVD, "mod file override read failed: %s -> %s\n",
                        override.modId.c_str(), normalizedPath.c_str());
                return;
            }
            if (copyCount < bytes.size()) {
                std::fill(bytes.begin() + copyCount, bytes.end(), 0);
            }
        }
        const auto key = std::make_pair(normalizedPath, UINT32_MAX);
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.loggedHits.insert(key).second) {
            RT_LOGF(RT_TAG_DVD,
                    "mod file override hit: %s (%s) -> %s payload=%zu read=0x%llx+%zu\n",
                    override.modId.c_str(), override.platform.c_str(), normalizedPath.c_str(),
                    static_cast<size_t>(override.payloadSize), static_cast<unsigned long long>(readStart), bytes.size());
        }
        return;
    }
    if (g_state.afsOverrides.empty()) {
        return;
    }

    bool virtualPlanRejected = false;
    const VirtualAfsPlan* virtualPlan =
        GetOrBuildVirtualAfsPlan(normalizedPath, hostPath, virtualPlanRejected);
    if (virtualPlanRejected) {
        return;
    }
    if (virtualPlan) {
        if (!ApplyVirtualAfsPlan(*virtualPlan, hostPath, fileOffset, bytes)) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.rejectedVirtualAfsPlans.insert(normalizedPath);
            RT_LOGF(RT_TAG_DVD,
                    "mod virtual AFS read failed: %s; preserving retail bytes for this read\n",
                    normalizedPath.c_str());
            return;
        }
        const auto key = std::make_pair(normalizedPath, UINT32_MAX - 1u);
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.loggedHits.insert(key).second) {
            RT_LOGF(RT_TAG_DVD, "mod virtual AFS first read hit: %s\n", normalizedPath.c_str());
        }
        return;
    }

    for (const AfsOverride& override : g_state.afsOverrides) {
        if (override.dvdPath != normalizedPath) {
            continue;
        }

        const auto key = std::make_pair(override.dvdPath, override.entryIndex);
        AfsLayout layout;
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            if (g_state.rejectedLayouts.count(key) != 0) {
                continue;
            }
            const auto cached = g_state.layoutCache.find(key);
            if (cached != g_state.layoutCache.end()) {
                layout = cached->second;
            } else {
                const auto parsed = ReadAfsLayout(hostPath, override.entryIndex);
                if (!parsed || override.payloadSize > parsed->byteSize) {
                    g_state.rejectedLayouts.insert(key);
                    if (!parsed) {
                        RT_LOGF(RT_TAG_DVD, "mod override rejected: %s cannot resolve %s[%u]\n",
                                override.modId.c_str(), override.dvdPath.c_str(), override.entryIndex);
                    } else {
                        RT_LOGF(RT_TAG_DVD,
                                "mod override rejected: %s payload=%zu exceeds %s[%u] slot=%u\n",
                                override.modId.c_str(), static_cast<size_t>(override.payloadSize),
                                override.dvdPath.c_str(), override.entryIndex, parsed->byteSize);
                    }
                    continue;
                }
                layout = *parsed;
                g_state.layoutCache.emplace(key, layout);
            }
        }

        const uint64_t readStart = fileOffset;
        const uint64_t readEnd = readStart + bytes.size();
        const uint64_t slotStart = layout.byteOffset;
        const uint64_t slotEnd = slotStart + layout.byteSize;
        const uint64_t overlapStart = std::max(readStart, slotStart);
        const uint64_t overlapEnd = std::min(readEnd, slotEnd);
        if (overlapStart >= overlapEnd) {
            continue;
        }

        const size_t destinationOffset = static_cast<size_t>(overlapStart - readStart);
        const uint64_t payloadOffset = overlapStart - slotStart;
        const size_t overlapCount = static_cast<size_t>(overlapEnd - overlapStart);
        const size_t payloadCount = payloadOffset < override.payloadSize
                                        ? std::min<size_t>(overlapCount,
                                                           static_cast<size_t>(override.payloadSize - payloadOffset))
                                        : 0;
        if (payloadCount != 0 &&
            !ReadPayloadRange(override.payloadPath,
                              override.payloadSize,
                              payloadOffset,
                              bytes.data() + destinationOffset,
                              payloadCount)) {
            RT_LOGF(RT_TAG_DVD, "mod override read failed: %s -> %s[%u]\n",
                    override.modId.c_str(), override.dvdPath.c_str(), override.entryIndex);
            return;
        }
        if (payloadCount < overlapCount) {
            std::fill(bytes.begin() + destinationOffset + payloadCount,
                      bytes.begin() + destinationOffset + overlapCount, 0);
        }

        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.loggedHits.insert(key).second) {
            RT_LOGF(RT_TAG_DVD,
                    "mod override hit: %s (%s) -> %s[%u] slot=%u payload=%zu\n",
                    override.modId.c_str(), override.platform.c_str(), override.dvdPath.c_str(),
                    override.entryIndex, layout.byteSize, static_cast<size_t>(override.payloadSize));
        }
    }
}

} // namespace RuntimeMods
