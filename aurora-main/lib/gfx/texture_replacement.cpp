#include "texture_replacement.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../webgpu/gpu.hpp"
#include "dds_io.hpp"
#include "texture_convert.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <SDL3/SDL_cpuinfo.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string_view>

#include "png_io.hpp"
#include "../fs_helper.hpp"

using namespace aurora::gx;
using aurora::webgpu::g_device;

namespace aurora::gfx::texture_replacement {
Module Log("aurora::gfx::texture_replacement");

struct RuntimeTextureKey {
  uint64_t textureHash = 0;
  uint64_t tlutHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool hasTlut = false;
  uint32_t format = 0;

  bool operator==(const RuntimeTextureKey& rhs) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const RuntimeTextureKey& key) {
    return H::combine(std::move(h), key.textureHash, key.tlutHash, key.width, key.height, key.hasTlut, key.format);
  }
};

struct TlutMetadata {
  uint32_t size = 0;
  uint32_t format = 0;
  uint16_t entries = 0;
  bool valid = false;
  // Object the palette was registered from. GXLoadTlut consumes the registration, so this is what
  // lets the same object be loaded into more than one hardware slot.
  const GXTlutObj* source = nullptr;
  ByteBuffer data;
};

struct CachedReplacement {
  gfx::TextureHandle handle;
  uint64_t bytes = 0;
  std::list<RuntimeTextureKey>::iterator lruIt;
};

struct ReplacementIndexEntry {
  std::filesystem::path path;
};

struct ReplacementShapeKey {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  bool hasTlut = false;

  bool operator==(const ReplacementShapeKey& rhs) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const ReplacementShapeKey& key) {
    return H::combine(std::move(h), key.width, key.height, key.format, key.hasTlut);
  }
};

enum class ControllerPromptStyle : size_t {
  GameCube = 0,
  Classic,
  Ps2,
  Count,
};

constexpr std::array<std::string_view, static_cast<size_t>(ControllerPromptStyle::Count)> kControllerPromptStyleNames{
    "gamecube", "classic", "ps2"};

absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry> s_replacementIndex;
absl::flat_hash_map<RuntimeTextureKey, CachedReplacement> s_replacementCache;
absl::flat_hash_set<RuntimeTextureKey> s_failedKeys;
absl::flat_hash_set<RuntimeTextureKey> s_reportedMisses;
absl::flat_hash_map<const GXTlutObj*, TlutMetadata> s_pendingTluts;
std::array<TlutMetadata, MaxTluts> s_loadedTluts{};
std::list<RuntimeTextureKey> s_replacementLru;
std::filesystem::path s_replacementRoot;
std::filesystem::path s_dumpRoot;
std::filesystem::path s_controllerPromptRoot;
std::array<absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry>,
           static_cast<size_t>(ControllerPromptStyle::Count)>
    s_controllerPromptIndexes;
std::array<absl::flat_hash_set<ReplacementShapeKey>, static_cast<size_t>(ControllerPromptStyle::Count)>
    s_controllerPromptShapes;
absl::flat_hash_map<RuntimeTextureKey, gfx::TextureHandle> s_controllerPromptCache;
absl::flat_hash_set<RuntimeTextureKey> s_controllerPromptFailedKeys;
absl::flat_hash_set<RuntimeTextureKey> s_controllerPromptTraceHits;
absl::flat_hash_set<RuntimeTextureKey> s_controllerPromptTraceMisses;
ControllerPromptStyle s_controllerPromptStyle = ControllerPromptStyle::GameCube;
bool s_reportedControllerPromptHit = false;
bool s_hasControllerPromptPack = false;
uint64_t s_replacementCacheBytes = 0;
constexpr uint64_t kReplacementCacheBudgetBytes = 4294967296; // 4GB, reasonable for modern hardware?
constexpr uint64_t kReplacementWildcardTextureHash = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t kReplacementWildcardTlutHash = 0xFFFFFFFFFFFFFFFEull;

uint64_t replacement_cache_budget_bytes() noexcept {
  static const uint64_t budget = [] {
    if (const char* value = std::getenv("AURORA_REPLACEMENT_CACHE_BUDGET_MB");
        value != nullptr && *value != '\0') {
      char* end = nullptr;
      const unsigned long long mib = std::strtoull(value, &end, 10);
      if (end != value && *end == '\0' && mib != 0 && mib <= (UINT64_MAX >> 20)) {
        Log.info("Texture replacement cache budget override: {} MiB", mib);
        return static_cast<uint64_t>(mib) << 20;
      }
      Log.warn("Ignoring invalid AURORA_REPLACEMENT_CACHE_BUDGET_MB='{}'", value);
    }

#if defined(AURORA_LOW_MEMORY_PIPELINE_PREWARM)
    // Keep replacement quality identical on constrained hosts; only bound how
    // many already-decoded GPU textures remain resident. Evicted replacements
    // are loaded again from the same full-quality source when reused.
    uint64_t automaticBudget = kReplacementCacheBudgetBytes;
    const int systemRamMiB = SDL_GetSystemRAM();
    if (systemRamMiB > 0 && systemRamMiB <= 4096) {
      automaticBudget = uint64_t{256} << 20;
    } else if (systemRamMiB > 0 && systemRamMiB <= 8192) {
      automaticBudget = uint64_t{512} << 20;
    }

    const uint64_t dedicatedVramMiB = webgpu::dedicated_video_memory_bytes() >> 20;
    uint64_t vramBudget = kReplacementCacheBudgetBytes;
    if (dedicatedVramMiB != 0 && dedicatedVramMiB <= 1024) {
      vramBudget = uint64_t{128} << 20;
    } else if (dedicatedVramMiB != 0 && dedicatedVramMiB <= 2048) {
      vramBudget = uint64_t{256} << 20;
    } else if (dedicatedVramMiB != 0 && dedicatedVramMiB <= 4096) {
      vramBudget = uint64_t{512} << 20;
    }
    automaticBudget = std::min(automaticBudget, vramBudget);

    if (automaticBudget != kReplacementCacheBudgetBytes) {
      Log.info("Low-memory replacement policy: systemRAM={} MiB dedicatedVRAM={} MiB -> cache budget {} MiB",
               systemRamMiB, dedicatedVramMiB, automaticBudget >> 20);
      return automaticBudget;
    }
#endif

    return kReplacementCacheBudgetBytes;
  }();
  return budget;
}

bool is_ps2_clash_stick_key(const RuntimeTextureKey& key) noexcept {
  if (key.width != 64 || key.height != 64 || key.format != GX_TF_C4) {
    return false;
  }
  constexpr std::array<uint64_t, 6> kTextureHashes{
      0xe63e59943072d5baull,
      0x0e45133195ab2db7ull,
      0x23a7ebdcad6f294cull,
      0xfc736187474b6d87ull,
      0x847d2a027ba61a6dull,
      0x2fd46476d6eeb2a5ull,
  };
  return std::find(kTextureHashes.begin(), kTextureHashes.end(), key.textureHash) != kTextureHashes.end();
}

bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool is_relative_to(const std::filesystem::path& path, const std::filesystem::path& root) noexcept {
  if (root.empty()) {
    return false;
  }
  auto pathIt = path.begin();
  auto rootIt = root.begin();
  for (; rootIt != root.end(); ++rootIt, ++pathIt) {
    if (pathIt == path.end() || !iequals_ascii(fs_path_to_string(*pathIt), fs_path_to_string(*rootIt))) {
      return false;
    }
  }
  return true;
}

bool is_sidecar_mip(std::string_view stem) noexcept {
  constexpr std::string_view tag = "_mip";
  size_t i = stem.size();
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') {
    --i;
  }

  if (i == stem.size() || i < tag.size()) {
    return false;
  }

  return stem.substr(i - tag.size(), tag.size()) == tag;
}

std::optional<uint64_t> parse_hex(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char ch : text) {
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<uint64_t>(ch - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

std::optional<uint32_t> parse_u32(std::string_view text, int base = 10) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }

  uint32_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value, base);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::pair<uint32_t, uint32_t>> parse_dimensions(std::string_view text) noexcept {
  const size_t sep = text.find('x');
  if (sep == std::string_view::npos) {
    return std::nullopt;
  }

  const auto width = parse_u32(text.substr(0, sep));
  const auto height = parse_u32(text.substr(sep + 1));
  if (!width.has_value() || !height.has_value()) {
    return std::nullopt;
  }
  return std::pair{*width, *height};
}

uint32_t texture_base_level_size(const GXTexObj_& obj) noexcept {
  switch (obj.format()) {
  case GX_TF_R8_PC:
    return obj.width() * obj.height();
  case GX_TF_RGBA8_PC:
    return obj.width() * obj.height() * 4;
  default:
    return GXGetTexBufferSize(obj.width(), obj.height(), obj.format(), false, 0);
  }
}

std::optional<uint64_t> compute_referenced_tlut_hash_from_data(const GXTexObj_& obj, const uint8_t* tlutData,
                                                               size_t tlutBytes) noexcept {
  if (!is_palette_format(obj.format()) || tlutData == nullptr || tlutBytes == 0) {
    return std::nullopt;
  }
  const uint32_t textureSize = texture_base_level_size(obj);
  const auto* textureData = static_cast<const uint8_t*>(obj.data);
  if (textureData == nullptr || textureSize == 0) {
    return std::nullopt;
  }

  uint32_t minIndex = 0xffff;
  uint32_t maxIndex = 0;
  switch (obj.format()) {
  case GX_TF_C4:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t lowNibble = textureData[i] & 0xf;
      const uint32_t highNibble = textureData[i] >> 4;
      minIndex = std::min({minIndex, lowNibble, highNibble});
      maxIndex = std::max({maxIndex, lowNibble, highNibble});
    }
    break;
  case GX_TF_C8:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t index = textureData[i];
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  case GX_TF_C14X2:
    for (uint32_t i = 0; i + sizeof(uint16_t) <= textureSize; i += sizeof(uint16_t)) {
      uint16_t value = 0;
      std::memcpy(&value, textureData + i, sizeof(value));
      const uint32_t index = bswap(value) & 0x3fff;
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  default:
    return std::nullopt;
  }

  size_t tlutSize = 2 * (static_cast<size_t>(maxIndex) + 1 - minIndex);
  const size_t tlutOffset = 2 * static_cast<size_t>(minIndex);
  if (tlutOffset + tlutSize > tlutBytes) {
    return std::nullopt;
  }
  return XXH64(tlutData + tlutOffset, tlutSize, 0);
}

std::optional<uint64_t> compute_referenced_tlut_hash(const GXTexObj_& obj) noexcept {
  if (!is_palette_format(obj.format()) || obj.tlut >= s_loadedTluts.size()) {
    return std::nullopt;
  }

  const auto& tlut = s_loadedTluts[obj.tlut];
  if (!tlut.valid) {
    return std::nullopt;
  }
  return compute_referenced_tlut_hash_from_data(obj, tlut.data.data(), tlut.data.size());
}

const TlutMetadata* get_loaded_tlut(const GXTexObj_& obj) noexcept {
  if (!is_palette_format(obj.format()) || obj.tlut >= s_loadedTluts.size()) {
    return nullptr;
  }

  const auto& tlut = s_loadedTluts[obj.tlut];
  return tlut.valid ? &tlut : nullptr;
}

bool ensure_directory(const std::filesystem::path& dir) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec;
}

RuntimeTextureKey build_runtime_key(const GXTexObj_& obj, const GXTlutObj_* directTlut = nullptr) noexcept {
  RuntimeTextureKey key{
      .width = obj.width(),
      .height = obj.height(),
      .hasTlut = is_palette_format(obj.format()),
      .format = obj.format(),
  };

  const uint32_t textureSize = texture_base_level_size(obj);
  if (obj.data != nullptr && textureSize != 0) {
    key.textureHash = XXH64(obj.data, textureSize, 0);
  }
  if (key.hasTlut) {
    if (directTlut != nullptr && directTlut->data != nullptr && directTlut->numEntries != 0) {
      key.tlutHash = compute_referenced_tlut_hash_from_data(
                         obj, static_cast<const uint8_t*>(directTlut->data),
                         static_cast<size_t>(directTlut->numEntries) * 2)
                         .value_or(0);
    } else {
      key.tlutHash = compute_referenced_tlut_hash(obj).value_or(0);
    }
  }
  return key;
}

std::string format_replacement_filename(const RuntimeTextureKey& key) {
  if (key.hasTlut) {
    return fmt::format("tex1_{}x{}_{:016x}_{:016x}_{}.dds", key.width, key.height,
                       key.textureHash, key.tlutHash, key.format);
  }
  return fmt::format("tex1_{}x{}_{:016x}_{}.dds", key.width, key.height, key.textureHash, key.format);
}

std::optional<RuntimeTextureKey> parse_replacement_filename(std::string_view filename) noexcept {
  const size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }

  if (!iequals_ascii(filename.substr(dot), ".dds") && !iequals_ascii(filename.substr(dot), ".png")) {
    return std::nullopt;
  }

  const std::string_view stem = filename.substr(0, dot);
  constexpr std::string_view prefix = "tex1_";
  if (!stem.starts_with(prefix)) {
    return std::nullopt;
  }

  std::array<std::string_view, 6> parts{};
  size_t partCount = 0;
  size_t offset = 0;
  bool consumedAll = false;
  while (offset <= stem.size() && partCount < parts.size()) {
    const size_t next = stem.find('_', offset);
    parts[partCount++] = stem.substr(offset, next == std::string_view::npos ? stem.size() - offset : next - offset);
    if (next == std::string_view::npos) {
      consumedAll = true;
      break;
    }
    offset = next + 1;
  }
  if (!consumedAll || partCount < 4 || partCount > 6 || parts[0] != "tex1") {
    return std::nullopt;
  }

  const auto dimensions = parse_dimensions(parts[1]);
  if (!dimensions.has_value()) {
    return std::nullopt;
  }

  size_t index = 2;
  if (parts[index] == "m") {
    ++index;
  }

  size_t remaining = partCount - index;
  if (remaining != 2 && remaining != 3) {
    return std::nullopt;
  }

  uint64_t textureHash = 0;
  if (parts[index] == "$") {
    textureHash = kReplacementWildcardTextureHash;
  } else {
    const auto parsedTex = parse_hex(parts[index]);
    if (!parsedTex.has_value()) {
      return std::nullopt;
    }
    textureHash = *parsedTex;
  }

  auto formatPart = parts[partCount - 1];
  if (formatPart == "arb") {
    formatPart = parts[partCount - 2];
    remaining -= 1;
  }
  const auto format = parse_u32(formatPart);
  if (!format.has_value()) {
    return std::nullopt;
  }

  uint64_t tlutHash = 0;
  const bool hasTlut = remaining == 3;
  if (hasTlut) {
    const std::string_view tlutPart = parts[index + 1];
    if (tlutPart == "$") {
      tlutHash = kReplacementWildcardTlutHash;
    } else {
      const auto parsedTlutHash = parse_hex(tlutPart);
      if (!parsedTlutHash.has_value()) {
        return std::nullopt;
      }
      tlutHash = *parsedTlutHash;
    }
  }

  return RuntimeTextureKey{
      .textureHash = textureHash,
      .tlutHash = tlutHash,
      .width = dimensions->first,
      .height = dimensions->second,
      .hasTlut = hasTlut,
      .format = *format,
  };
}

using ReplacementIndex = absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry>;

void index_replacement_directory(const std::filesystem::path& root, ReplacementIndex& index) noexcept {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec) {
    return;
  }

  for (std::filesystem::recursive_directory_iterator it(
           root,
           std::filesystem::directory_options::skip_permission_denied |
               std::filesystem::directory_options::follow_directory_symlink,
           ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (!it->is_regular_file()) {
      continue;
    }

    const auto& path = it->path();
    if (!iequals_ascii(fs_path_to_string(path.extension()), ".dds") &&
        !iequals_ascii(fs_path_to_string(path.extension()), ".png")) {
      continue;
    }
    if (is_sidecar_mip(fs_path_to_string(path.stem()))) {
      continue;
    }

    const auto parsed = parse_replacement_filename(fs_path_to_string(path.filename()));
    if (!parsed.has_value()) {
      continue;
    }
    index.try_emplace(*parsed, path);
  }
}

bool has_controller_prompt_pack() noexcept {
  return s_hasControllerPromptPack;
}

ReplacementIndex& active_controller_prompt_index() noexcept {
  return s_controllerPromptIndexes[static_cast<size_t>(s_controllerPromptStyle)];
}

const absl::flat_hash_set<ReplacementShapeKey>& active_controller_prompt_shapes() noexcept {
  return s_controllerPromptShapes[static_cast<size_t>(s_controllerPromptStyle)];
}

ReplacementShapeKey replacement_shape_key(const GXTexObj_& obj) noexcept {
  return {
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
      .hasTlut = is_palette_format(obj.format()),
  };
}

static std::optional<ConvertedTexture> load_texture_file(const std::filesystem::path& path) {
  if (path.extension() == ".png") {
    return png::load_png_file(path);
  } else {
    return dds::load_dds_file(path);
  }
}

constexpr bool isUnsupportedTextureFormat(const ConvertedTexture& texture) {
  switch (texture.format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
  case wgpu::TextureFormat::BC4RUnorm:
  case wgpu::TextureFormat::BC4RSnorm:
  case wgpu::TextureFormat::BC5RGUnorm:
  case wgpu::TextureFormat::BC5RGSnorm:
  case wgpu::TextureFormat::BC6HRGBUfloat:
  case wgpu::TextureFormat::BC6HRGBFloat:
  case wgpu::TextureFormat::BC7RGBAUnorm:
  case wgpu::TextureFormat::BC7RGBAUnormSrgb:
    return !webgpu::g_bcTexturesSupported;
  default:
    return false;
  }
}

std::optional<ConvertedTexture> load_replacement(const ReplacementIndexEntry& entry) noexcept {
  auto base = load_texture_file(entry.path);
  if (!base.has_value()) {
    Log.warn("texture_replacement: failed to load texture {}", fs_path_to_string(entry.path));
    return std::nullopt;
  }
  if (isUnsupportedTextureFormat(base.value())) {
    Log.warn(
      "texture_replacement: failed to load texture {} due to unsupported format: {}",
      fs_path_to_string(entry.path),
      static_cast<uint32_t>(base->format));
    return std::nullopt;
  }

  std::vector<ConvertedTexture> more;
  std::error_code ec;
  for (uint32_t mipLevel = 1;; ++mipLevel) {
    const auto mipPath = entry.path.parent_path() / fmt::format("{}_mip{}{}", fs_path_to_string(entry.path.stem()), mipLevel, fs_path_to_string(entry.path.extension()));
    if (!std::filesystem::is_regular_file(mipPath, ec)) {
      break;
    }

    auto lvl = load_texture_file(mipPath);
    const uint32_t ew = std::max(base->width >> mipLevel, 1u);
    const uint32_t eh = std::max(base->height >> mipLevel, 1u);
    const bool ok = lvl.has_value() && lvl->format == base->format && lvl->width == ew && lvl->height == eh;
    if (!ok) {
      if (!lvl.has_value()) {
        Log.warn("texture_replacement: could not load mip {}", fs_path_to_string(mipPath));
      } else {
        Log.warn("texture_replacement: expected {}x{} for mip {}, got {}x{}", ew, eh, fs_path_to_string(mipPath),
                 lvl->width, lvl->height);
      }

      break;
    }
    more.push_back(std::move(*lvl));
  }

  if (more.empty()) {
    return base;
  }

  const uint32_t mips = 1u + static_cast<uint32_t>(more.size());
  const uint64_t n = calc_texture_size(base->format, base->width, base->height, mips);
  if (n == 0) {
    return std::nullopt;
  }

  ByteBuffer blob{static_cast<size_t>(n)};
  uint8_t* const dst = blob.data();
  uint64_t o = 0;
  const auto append = [&](const ByteBuffer& d) noexcept -> bool {
    if (o + d.size() > n) {
      return false;
    }
    std::memcpy(dst + o, d.data(), d.size());
    o += d.size();
    return true;
  };
  if (!append(base->data)) {
    return std::nullopt;
  }
  for (const auto& mip : more) {
    if (!append(mip.data)) {
      return std::nullopt;
    }
  }
  if (o != n) {
    return std::nullopt;
  }

  return ConvertedTexture{
      .format = base->format,
      .width = base->width,
      .height = base->height,
      .mips = mips,
      .data = std::move(blob),
  };
}

void touch_cached_replacement(decltype(s_replacementCache)::iterator it) noexcept {
  if (it->second.lruIt != s_replacementLru.begin()) {
    s_replacementLru.splice(s_replacementLru.begin(), s_replacementLru, it->second.lruIt);
    it->second.lruIt = s_replacementLru.begin();
  }
}

void evict_replacement_cache_if_needed() noexcept {
  const uint64_t budget = replacement_cache_budget_bytes();
  while (s_replacementCacheBytes > budget && !s_replacementLru.empty()) {
    const RuntimeTextureKey key = s_replacementLru.back();
    s_replacementLru.pop_back();

    const auto it = s_replacementCache.find(key);
    if (it == s_replacementCache.end()) {
      continue;
    }

    const uint64_t entryBytes = it->second.bytes;
    s_replacementCache.erase(it);
    s_replacementCacheBytes -= std::min(s_replacementCacheBytes, entryBytes);
  }
}

void build_index() noexcept {
  if (!g_config.allowTextureReplacements) {
    return;
  }

  auto userPath = fs_path_from_string(g_config.userPath);
  auto cachePath = fs_path_from_string(g_config.cachePath);

  s_replacementRoot = userPath / "texture_replacements";
  s_dumpRoot = cachePath / "texture_dumps";

  if (!ensure_directory(s_replacementRoot)) {
    return;
  }
  if (g_config.allowTextureDumps && !ensure_directory(s_dumpRoot)) {
    return;
  }

  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(
           s_replacementRoot,
           std::filesystem::directory_options::skip_permission_denied |
               std::filesystem::directory_options::follow_directory_symlink,
           ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }

    if (!it->is_regular_file()) {
      continue;
    }

    const auto& path = it->path();

    if (is_relative_to(path, s_dumpRoot)) {
      continue;
    }

    if (!iequals_ascii(fs_path_to_string(path.extension()), ".dds") && !iequals_ascii(fs_path_to_string(path.extension()), ".png")) {
      continue;
    }

    if (is_sidecar_mip(fs_path_to_string(path.stem()))) {
      continue;
    }

    const auto parsed = parse_replacement_filename(fs_path_to_string(path.filename()));
    if (!parsed.has_value()) {
      continue;
    }

    s_replacementIndex.try_emplace(*parsed, path);
  }

  Log.info("Indexed {} texture replacements", s_replacementIndex.size());
}

void build_controller_prompt_indexes() noexcept {
  if (g_config.resourcesPath == nullptr || g_config.resourcesPath[0] == '\0') {
    return;
  }

  s_controllerPromptRoot = fs_path_from_string(g_config.resourcesPath) / "controller_prompts";
  size_t total = 0;
  for (size_t i = 0; i < kControllerPromptStyleNames.size(); ++i) {
    auto& index = s_controllerPromptIndexes[i];
    auto& shapes = s_controllerPromptShapes[i];
    index.clear();
    shapes.clear();
    index_replacement_directory(s_controllerPromptRoot / kControllerPromptStyleNames[i], index);
    for (const auto& [key, entry] : index) {
      (void)entry;
      shapes.insert({
          .width = key.width,
          .height = key.height,
          .format = key.format,
          .hasTlut = key.hasTlut,
      });
    }
    total += index.size();
    Log.info("Indexed {} {} controller prompt replacements across {} texture shapes", index.size(),
             kControllerPromptStyleNames[i], shapes.size());
  }
  s_hasControllerPromptPack = total != 0;
  if (total != 0) {
    Log.info("Indexed {} built-in controller prompt replacements", total);
  }
}

const ReplacementIndexEntry* find_replacement_path(const ReplacementIndex& index, const RuntimeTextureKey& key) noexcept {
  if (const auto it = index.find(key); it != index.end()) {
    return &it->second;
  }

  if (key.hasTlut) {
    RuntimeTextureKey tlutWildcardKey = key;
    tlutWildcardKey.tlutHash = kReplacementWildcardTlutHash;
    if (const auto it = index.find(tlutWildcardKey); it != index.end()) {
      return &it->second;
    }
  }

  RuntimeTextureKey textureWildcardKey = key;
  textureWildcardKey.textureHash = kReplacementWildcardTextureHash;
  if (const auto it = index.find(textureWildcardKey); it != index.end()) {
    return &it->second;
  }

  return nullptr;
}

const ReplacementIndexEntry* find_controller_prompt_path(const RuntimeTextureKey& key) noexcept {
  auto& index = active_controller_prompt_index();
  if (const auto* exact = find_replacement_path(index, key); exact != nullptr) {
    return exact;
  }

  // The game's CI texture payload is the stable identity of a prompt. Some
  // runtime paths load the same resident glyph with a different TLUT slice than
  // the static package descriptor, so accept a unique exact texture-hash match
  // inside the built-in prompt pack while still requiring dimensions/format.
  const ReplacementIndexEntry* textureOnly = nullptr;
  for (const auto& [candidate, entry] : index) {
    if (candidate.textureHash != key.textureHash || candidate.width != key.width ||
        candidate.height != key.height || candidate.format != key.format ||
        candidate.hasTlut != key.hasTlut) {
      continue;
    }
    if (textureOnly != nullptr && textureOnly->path != entry.path) {
      textureOnly = nullptr;
      break;
    }
    textureOnly = &entry;
  }
  if (textureOnly != nullptr) {
    Log.debug("Controller prompt matched texture hash with runtime TLUT {:016x}: {}", key.tlutHash,
              fs_path_to_string(textureOnly->path));
    return textureOnly;
  }
  return nullptr;
}

const gfx::TextureHandle* find_cached_replacement(const RuntimeTextureKey& key) noexcept {
  const auto cached = s_replacementCache.find(key);
  if (cached == s_replacementCache.end()) {
    return nullptr;
  }

  touch_cached_replacement(cached);
  return &cached->second.handle;
}

gfx::TextureHandle load_replacement_texture(const RuntimeTextureKey& key, const ReplacementIndexEntry& entry) noexcept {
  const auto replacement = load_replacement(entry);
  if (!replacement.has_value()) {
    return {};
  }

  const auto label = fmt::format("TextureReplacement {}", format_replacement_filename(key));
  const wgpu::Extent3D size{
      .width = replacement->width,
      .height = replacement->height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label.c_str(),
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = replacement->format,
      .mipLevelCount = replacement->mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = replacement->format,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = replacement->mips,
  };
  auto textureView = texture.CreateView(&textureViewDescriptor);
  auto handle = std::make_shared<gfx::TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size,
                                                  replacement->format, replacement->mips, gfx::InvalidTextureFormat);
  handle->isReplacement = true;
  gfx::write_texture(*handle, replacement->data);
  return handle;
}

void cache_replacement(const RuntimeTextureKey& key, const gfx::TextureHandle& handle) noexcept {
  const uint64_t replacementBytes =
      calc_texture_size(handle->format, handle->size.width, handle->size.height, handle->mipCount);
  s_replacementLru.push_front(key);
  s_replacementCache.emplace(
      key, CachedReplacement{.handle = handle, .bytes = replacementBytes, .lruIt = s_replacementLru.begin()});
  s_replacementCacheBytes += replacementBytes;
  evict_replacement_cache_if_needed();
}

bool dump_editable_texture_dds(const RuntimeTextureKey& key, const GXTexObj_& obj) noexcept {
  const ArrayRef<uint8_t> texData{static_cast<const uint8_t*>(obj.data), UINT32_MAX};
  const uint32_t texWidth = obj.width();
  const uint32_t texHeight = obj.height();

  ConvertedTexture pixels;
  if (is_palette_format(obj.format())) {
    const TlutMetadata* tlut = get_loaded_tlut(obj);
    if (tlut == nullptr) {
      return false;
    }
    pixels =
        convert_texture_palette(obj.format(), texWidth, texHeight, 1, texData, static_cast<GXTlutFmt>(tlut->format),
                                tlut->entries, {tlut->data.data(), tlut->data.size()});
  } else {
    pixels = convert_texture(obj.format(), texWidth, texHeight, 1, texData);
  }

  const uint64_t rgbaBytes = calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, texWidth, texHeight, 1);

  if (pixels.data.empty() || pixels.format != wgpu::TextureFormat::RGBA8Unorm || pixels.data.size() != rgbaBytes) {
    return false;
  }

  const auto path = s_dumpRoot / format_replacement_filename(key);
  return dds::write_rgba8_dds(path, texWidth, texHeight, pixels.data);
}

bool dump_controller_prompt_candidate(const RuntimeTextureKey& key, const GXTexObj_& obj,
                                      const GXTlutObj_* directTlut) noexcept {
  if (g_config.cachePath == nullptr || g_config.cachePath[0] == '\0') {
    return false;
  }

  ConvertedTexture pixels;
  const uint32_t texWidth = obj.width();
  const uint32_t texHeight = obj.height();
  const uint32_t dataSize = texture_base_level_size(obj);
  if (obj.data == nullptr || dataSize == 0) {
    return false;
  }
  const ArrayRef<uint8_t> texData{static_cast<const uint8_t*>(obj.data), dataSize};

  if (is_palette_format(obj.format())) {
    const uint8_t* paletteData = nullptr;
    size_t paletteBytes = 0;
    GXTlutFmt paletteFormat = GX_TL_RGB5A3;
    uint16_t paletteEntries = 0;
    if (directTlut != nullptr && directTlut->data != nullptr && directTlut->numEntries != 0) {
      paletteData = static_cast<const uint8_t*>(directTlut->data);
      paletteEntries = directTlut->numEntries;
      paletteBytes = static_cast<size_t>(paletteEntries) * 2;
      paletteFormat = directTlut->format;
    } else if (const TlutMetadata* tlut = get_loaded_tlut(obj); tlut != nullptr) {
      paletteData = tlut->data.data();
      paletteBytes = tlut->data.size();
      paletteEntries = tlut->entries;
      paletteFormat = static_cast<GXTlutFmt>(tlut->format);
    }
    if (paletteData == nullptr || paletteBytes == 0 || paletteEntries == 0) {
      return false;
    }
    pixels = convert_texture_palette(obj.format(), texWidth, texHeight, 1, texData, paletteFormat,
                                     paletteEntries, {paletteData, paletteBytes});
  } else {
    pixels = convert_texture(obj.format(), texWidth, texHeight, 1, texData);
  }

  const uint64_t rgbaBytes = calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, texWidth, texHeight, 1);
  if (pixels.data.empty() || pixels.format != wgpu::TextureFormat::RGBA8Unorm || pixels.data.size() != rgbaBytes) {
    return false;
  }

  const auto traceRoot = fs_path_from_string(g_config.cachePath) / "controller_prompt_trace";
  if (!ensure_directory(traceRoot)) {
    return false;
  }
  const auto path = traceRoot / format_replacement_filename(key);
  return dds::write_rgba8_dds(path, texWidth, texHeight, pixels.data);
}

bool report_missing_key(const RuntimeTextureKey& key, const GXTexObj_& obj) noexcept {
  if (!s_reportedMisses.insert(key).second) {
    return false;
  }

  if (g_config.allowTextureDumps) {
    dump_editable_texture_dds(key, obj);
  }
  return true;
}

void initialize() noexcept {
  build_index();
  build_controller_prompt_indexes();
}

void shutdown() noexcept {
  s_replacementIndex.clear();
  s_replacementCache.clear();
  s_failedKeys.clear();
  s_reportedMisses.clear();
  for (auto& index : s_controllerPromptIndexes) {
    index.clear();
  }
  for (auto& shapes : s_controllerPromptShapes) {
    shapes.clear();
  }
  s_controllerPromptCache.clear();
  s_controllerPromptFailedKeys.clear();
  s_controllerPromptTraceHits.clear();
  s_controllerPromptTraceMisses.clear();
  s_pendingTluts.clear();
  for (auto& tlut : s_loadedTluts) {
    tlut = {};
  }
  s_replacementLru.clear();
  s_replacementCacheBytes = 0;
  s_replacementRoot.clear();
  s_dumpRoot.clear();
  s_controllerPromptRoot.clear();
  s_controllerPromptStyle = ControllerPromptStyle::GameCube;
  s_reportedControllerPromptHit = false;
  s_hasControllerPromptPack = false;
}

// Mirrors of the guest palettes, kept only so find_replacement can hash and the DDS dump decode a
// CI palette. The renderer uploads from the guest pointer, so skip this work when replacement is off.
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept {
  if (!g_config.allowTextureReplacements && !has_controller_prompt_pack()) {
    return;
  }
  if (obj == nullptr || data == nullptr) {
    return;
  }

  // A registration is only consumed by a matching load_tlut, and the map is keyed by a guest address
  // the game may free and reuse, so a table this large means stale entries: drop them wholesale.
  constexpr size_t kMaxPendingTluts = 4096;
  if (s_pendingTluts.size() >= kMaxPendingTluts && !s_pendingTluts.contains(obj)) {
    s_pendingTluts.clear();
  }

  const size_t sz = static_cast<size_t>(entries) * 2;
  ByteBuffer buffer{sz};
  std::memcpy(buffer.data(), static_cast<const uint8_t*>(data), sz);
  s_pendingTluts[obj] = {
      .size = static_cast<uint32_t>(entries) * 2,
      .format = static_cast<uint32_t>(format),
      .entries = entries,
      .valid = true,
      .source = obj,
      .data = std::move(buffer),
  };
}

void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept {
  if (!g_config.allowTextureReplacements && !has_controller_prompt_pack()) {
    return;
  }
  if (idx >= s_loadedTluts.size()) {
    return;
  }

  // Consume the pending registration. The map is keyed by a guest object address that can be freed
  // or reused at any time, and entries used to live until shutdown.
  if (const auto it = s_pendingTluts.find(obj); it != s_pendingTluts.end()) {
    s_loadedTluts[idx] = std::move(it->second);
    s_pendingTluts.erase(it);
    return;
  }

  // The same object may legally be loaded into several hardware slots; only the first load finds the
  // pending entry, so recover the palette from whichever slot already holds it.
  for (const auto& loaded : s_loadedTluts) {
    if (!loaded.valid || loaded.source != obj) {
      continue;
    }
    s_loadedTluts[idx] = {
        .size = loaded.size,
        .format = loaded.format,
        .entries = loaded.entries,
        .valid = loaded.valid,
        .source = loaded.source,
        .data = loaded.data.clone(),
    };
    return;
  }

  s_loadedTluts[idx] = {};
}

std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, const GXTlutObj_* directTlut) noexcept {
  ZoneScoped;

  if (!g_config.allowTextureReplacements && !has_controller_prompt_pack()) {
    return std::nullopt;
  }
  // Built-in prompt matching always requires exact dimensions, GX format and
  // palette/non-palette class. In the normal configuration user replacements
  // are disabled, so reject impossible prompt shapes before hashing the full
  // guest texture (and its TLUT) on every static-texture resolve.
  if (!g_config.allowTextureReplacements &&
      !active_controller_prompt_shapes().contains(replacement_shape_key(obj))) {
    return std::nullopt;
  }

  const RuntimeTextureKey key = build_runtime_key(obj, directTlut);
  if (g_config.allowTextureReplacements) {
    if (const auto* path = find_replacement_path(s_replacementIndex, key); path != nullptr) {
      if (const auto* cached = find_cached_replacement(key); cached != nullptr) {
        return *cached;
      }
      if (s_failedKeys.contains(key)) {
        return std::nullopt;
      }

      auto handle = load_replacement_texture(key, *path);
      if (!handle) {
        s_failedKeys.insert(key);
        return std::nullopt;
      }
      cache_replacement(key, handle);
      return handle;
    }

    // Preserve normal user texture-dump behavior even when a built-in prompt
    // replacement satisfies this texture afterwards.
    report_missing_key(key, obj);
  }

  if (const auto* path = find_controller_prompt_path(key); path != nullptr) {
    if (const auto cached = s_controllerPromptCache.find(key); cached != s_controllerPromptCache.end()) {
      return cached->second;
    }
    if (s_controllerPromptFailedKeys.contains(key)) {
      return std::nullopt;
    }

    auto handle = load_replacement_texture(key, *path);
    if (!handle) {
      s_controllerPromptFailedKeys.insert(key);
      return std::nullopt;
    }
    // Battle_SP's PAL PS2 clash-stick frames have their own grayscale RGB
    // shading.  The Wii draw tints the stock mask green, so tag only these six
    // exact source textures for the GX draw bridge to preserve replacement RGB
    // while retaining the retail alpha/fade path.
    if (s_controllerPromptStyle == ControllerPromptStyle::Ps2 && is_ps2_clash_stick_key(key)) {
      handle->preserveReplacementRgb = true;
    }
    s_controllerPromptCache.emplace(key, handle);
    if (std::getenv("METEOR_PROMPT_TRACE") != nullptr &&
        s_controllerPromptTraceHits.insert(key).second) {
      Log.info("Matched controller prompt key {} texObjId={} data={}",
               format_replacement_filename(key), obj.texObjId, obj.data);
    }
    if (!s_reportedControllerPromptHit) {
      s_reportedControllerPromptHit = true;
      Log.info("Applied built-in {} controller prompt replacement {}",
               kControllerPromptStyleNames[static_cast<size_t>(s_controllerPromptStyle)],
               format_replacement_filename(key));
    }
    return handle;
  }

  if (std::getenv("METEOR_PROMPT_TRACE") != nullptr && key.format == GX_TF_C4 &&
      (key.width == 32 || key.width == 64 || key.width == 128) &&
      (key.height == 32 || key.height == 64 || key.height == 128) &&
      s_controllerPromptTraceMisses.size() < 160 && s_controllerPromptTraceMisses.insert(key).second) {
    Log.info("Unmatched controller prompt candidate {}", format_replacement_filename(key));
    dump_controller_prompt_candidate(key, obj, directTlut);
  }

  return std::nullopt;
}

std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept {
  const RuntimeTextureKey key = build_runtime_key(obj);
  return format_replacement_filename(key);
}

bool set_controller_prompt_style(std::string_view style) noexcept {
  const auto it = std::find(kControllerPromptStyleNames.begin(), kControllerPromptStyleNames.end(), style);
  if (it == kControllerPromptStyleNames.end()) {
    return false;
  }

  const auto next = static_cast<ControllerPromptStyle>(std::distance(kControllerPromptStyleNames.begin(), it));
  if (s_controllerPromptIndexes[static_cast<size_t>(next)].empty()) {
    Log.error("Controller prompt pack '{}' is empty or unavailable", style);
    return false;
  }
  if (next == s_controllerPromptStyle) {
    return true;
  }

  s_controllerPromptStyle = next;
  s_controllerPromptCache.clear();
  s_controllerPromptFailedKeys.clear();
  s_controllerPromptTraceHits.clear();
  s_controllerPromptTraceMisses.clear();
  s_reportedControllerPromptHit = false;
  aurora::gx::invalidate_static_texture_cache();
  Log.info("Controller prompt style changed to {}", style);
  return true;
}

} // namespace aurora::gfx::texture_replacement

extern "C" bool aurora_set_controller_prompt_style(const char* style) {
  if (style == nullptr) {
    return false;
  }
  return aurora::gfx::texture_replacement::set_controller_prompt_style(style);
}
