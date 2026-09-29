#include "ppc_runtime.h"
#include "runtime_config.h"

#include <cstdint>
#include <string>

// RDSPAF converts the Wii language to its own five PAL resource-bank indices in
// 0x80027780. Keep that retail conversion as the default, and only replace the
// result when the user explicitly selected a language in WiiCompiled.
//
// Retail mapping proved by Ghidra HEADLESS:
//   English -> 0, Spanish -> 1, German -> 2, French -> 3, Italian -> 4.
// Dutch and unsupported Wii languages fall back to the English bank in retail.
void Meteor_ApplyConfiguredLanguageResult(CpuContext* ctx) noexcept {
    if (!ctx) {
        return;
    }

    const std::string language = RuntimeConfigFile::GameLanguage("system");
    if (language == "system") {
        return;
    }

    uint32_t resourceBank = 0u;
    if (language == "spanish") {
        resourceBank = 1u;
    } else if (language == "german") {
        resourceBank = 2u;
    } else if (language == "french") {
        resourceBank = 3u;
    } else if (language == "italian") {
        resourceBank = 4u;
    }
    // English intentionally remains bank 0.
    ctx->gpr[3] = resourceBank;
}
