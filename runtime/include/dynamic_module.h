#pragma once

#include <cstddef>
#include <cstdint>

#include "isa/ppc_isa_context.h"

namespace DynamicModule {

using RawCpuInvoker = void (*)(CpuContext*);

struct InstanceLayout {
    // Resolvers are also called from the generic icbi publication seam. The
    // probed cache-line address need not be the RSO image base, so the resolver
    // returns the exact validated image base here.
    uint32_t imageBase = 0;
    uint32_t bssBase = 0;
};

using InstanceResolver = bool (*)(uint32_t runtimeBase, InstanceLayout* layout);

struct FunctionRecord {
    uint32_t offset = 0;
    RawCpuInvoker entry = nullptr;
    bool preserveNonvolatileGprs = true;
    uint32_t nonvolatileFprWriteMask = 0;
};

struct TemplateRecord {
    uint8_t preLinkSha256[32]{};
    uint32_t canonicalBase = 0;
    uint32_t imageSize = 0;
    uint32_t canonicalBssBase = 0;
    uint32_t bssSize = 0;
    const FunctionRecord* functions = nullptr;
    size_t functionCount = 0;
    InstanceResolver resolveInstance = nullptr;
    const char* debugName = nullptr;
    // Optional exact-base validator. Discovery must still try all publication
    // offsets, but a registered instance already owns a proved image base.
    // This validates the same complete signature/layout without rediscovery.
    InstanceResolver validateInstance = nullptr;
    // Optional cheap steady-state validator used by dispatch lookups after the
    // instance has already passed the complete publication/attach validation.
    // It should verify stable identity/layout fields (including BSS) without
    // rescanning the whole image. If absent, validateInstance/resolveInstance is
    // used so existing templates keep their fail-closed behavior.
    InstanceResolver validateDispatch = nullptr;
};

struct ResolvedTarget {
    const TemplateRecord* module = nullptr;
    const FunctionRecord* function = nullptr;
    uint32_t runtimeBase = 0;
    uint32_t runtimeBssBase = 0;
    uint64_t generation = 0;

    explicit operator bool() const noexcept {
        return module != nullptr && function != nullptr && function->entry != nullptr;
    }
};

struct InstanceToken {
    const TemplateRecord* module = nullptr;
    uint32_t runtimeBase = 0;
    uint64_t generation = 0;

    explicit operator bool() const noexcept {
        return module != nullptr && generation != 0;
    }
};

// Templates are immutable AOT-owned records. Runtime instances remain in a
// separate namespace from the fixed translated-function registry.
bool RegisterTemplate(const TemplateRecord* moduleTemplate);
bool AttachInstance(const TemplateRecord* moduleTemplate, uint32_t runtimeBase, InstanceToken& token);
bool DetachInstance(const InstanceToken& token);
// Generic publication seam for PowerPC icbi. If exactly one registered
// template validates at this address, publish/re-publish that instance with a
// fresh generation. Re-publication is what prevents same-base heap reuse ABA.
bool NotifyCodePublication(uint32_t candidateBase);
bool TryResolveTarget(uint32_t target, ResolvedTarget& resolved);
bool TryResolveCurrentFunction(const CpuContext* cpu, uint32_t functionOffset, ResolvedTarget& resolved);
bool CanDispatchTarget(uint32_t target);

// Product templates normally live for process lifetime. This hook exists only
// for isolated runtime tests.
void ResetRegistryForTests();

class ScopedExecution {
public:
    ScopedExecution(CpuContext* cpu, const ResolvedTarget& resolved) noexcept;
    ~ScopedExecution() noexcept;

    ScopedExecution(const ScopedExecution&) = delete;
    ScopedExecution& operator=(const ScopedExecution&) = delete;

private:
    CpuContext* cpu_ = nullptr;
    const void* previousTemplate_ = nullptr;
    uint32_t previousRuntimeBase_ = 0;
    uint32_t previousCanonicalBase_ = 0;
    uint32_t previousImageSize_ = 0;
    uint32_t previousRuntimeBssBase_ = 0;
    uint32_t previousCanonicalBssBase_ = 0;
    uint32_t previousBssSize_ = 0;
    uint64_t previousGeneration_ = 0;
};

inline uint32_t RebaseCanonicalAddress(const CpuContext* cpu, uint32_t address) noexcept {
    if (cpu == nullptr || cpu->dynamicModuleTemplate == nullptr || cpu->dynamicModuleImageSize == 0) {
        return address;
    }

    const uint64_t canonicalEnd =
        static_cast<uint64_t>(cpu->dynamicModuleCanonicalBase) + cpu->dynamicModuleImageSize;
    if (address < cpu->dynamicModuleCanonicalBase || static_cast<uint64_t>(address) >= canonicalEnd) {
        const uint64_t canonicalBssEnd =
            static_cast<uint64_t>(cpu->dynamicModuleCanonicalBssBase) + cpu->dynamicModuleBssSize;
        if (cpu->dynamicModuleBssSize != 0 &&
            address >= cpu->dynamicModuleCanonicalBssBase &&
            static_cast<uint64_t>(address) < canonicalBssEnd) {
            return cpu->dynamicModuleRuntimeBssBase + (address - cpu->dynamicModuleCanonicalBssBase);
        }
        return address;
    }
    return cpu->dynamicModuleRuntimeBase + (address - cpu->dynamicModuleCanonicalBase);
}

inline uint32_t RuntimeAddressFromOffset(const CpuContext* cpu, uint32_t moduleOffset) noexcept {
    if (cpu == nullptr || cpu->dynamicModuleTemplate == nullptr) {
        return moduleOffset;
    }
    return cpu->dynamicModuleRuntimeBase + moduleOffset;
}

inline uint32_t CanonicalAddressFromRuntime(const CpuContext* cpu, uint32_t address) noexcept {
    if (cpu == nullptr || cpu->dynamicModuleTemplate == nullptr) {
        return address;
    }
    const uint64_t runtimeImageEnd =
        static_cast<uint64_t>(cpu->dynamicModuleRuntimeBase) + cpu->dynamicModuleImageSize;
    if (address >= cpu->dynamicModuleRuntimeBase && static_cast<uint64_t>(address) < runtimeImageEnd) {
        return cpu->dynamicModuleCanonicalBase + (address - cpu->dynamicModuleRuntimeBase);
    }
    const uint64_t runtimeBssEnd =
        static_cast<uint64_t>(cpu->dynamicModuleRuntimeBssBase) + cpu->dynamicModuleBssSize;
    if (cpu->dynamicModuleBssSize != 0 &&
        address >= cpu->dynamicModuleRuntimeBssBase &&
        static_cast<uint64_t>(address) < runtimeBssEnd) {
        return cpu->dynamicModuleCanonicalBssBase + (address - cpu->dynamicModuleRuntimeBssBase);
    }
    return address;
}

// LR is architecturally the address after a linked branch. For a branch in the
// final instruction of a template image that value is exactly one-past-image,
// which must still move with the runtime instance even though it is not a
// callable code address.
inline uint32_t CanonicalLinkAddressFromRuntime(const CpuContext* cpu, uint32_t address) noexcept {
    if (cpu == nullptr || cpu->dynamicModuleTemplate == nullptr) {
        return address;
    }
    const uint64_t runtimeImageEnd =
        static_cast<uint64_t>(cpu->dynamicModuleRuntimeBase) + cpu->dynamicModuleImageSize;
    if (address >= cpu->dynamicModuleRuntimeBase &&
        static_cast<uint64_t>(address) <= runtimeImageEnd) {
        return cpu->dynamicModuleCanonicalBase + (address - cpu->dynamicModuleRuntimeBase);
    }
    return CanonicalAddressFromRuntime(cpu, address);
}

} // namespace DynamicModule

extern "C" void PPC_Icbi(uint32_t address);
