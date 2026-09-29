#include "dynamic_module.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(MKW_GENERIC_DOL_BOOT)
#include "memory.h"
#include <cstdio>
#include <cstring>
#endif

namespace DynamicModule {
namespace {

struct InstanceRecord {
    const TemplateRecord* module = nullptr;
    uint32_t runtimeBase = 0;
    uint32_t runtimeBssBase = 0;
    uint64_t generation = 0;
};

std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<const TemplateRecord*>& Templates() {
    static std::vector<const TemplateRecord*> templates;
    return templates;
}

std::unordered_map<uint32_t, InstanceRecord>& Instances() {
    static std::unordered_map<uint32_t, InstanceRecord> instances;
    return instances;
}

std::atomic<uint64_t>& NextGeneration() {
    static std::atomic<uint64_t> next{1};
    return next;
}

bool RangesOverlap(uint32_t aBase, uint32_t aSize, uint32_t bBase, uint32_t bSize);

#if defined(MKW_GENERIC_DOL_BOOT)
// Opt-in, bounded call/argument capture for one named module. It never changes
// guest memory or dispatch. The module name comes from the request, not a title
// address, so the same diagnostic remains valid at arbitrary allocation bases.
struct TraceCall { uint32_t id; uint32_t addresses[5]; };
struct ModuleTrace {
    FILE* output = nullptr;
    char name[128]{};
    uint32_t remaining = 0, nextId = 0, poll = 0, startOffset = 0;
    bool started = false;
    std::unordered_map<const void*, TraceCall> calls;
};
thread_local ModuleTrace g_moduleTrace;

void WriteTraceState(const char* phase, const TraceCall& call, const CpuContext* cpu, uint32_t offset) {
    auto* out = g_moduleTrace.output;
    std::fprintf(out, "%s id=%u offset=%08X lr=%08X cr=%08X\n", phase, call.id, offset, cpu->lr, cpu->cr);
    std::fprintf(out, " gpr");
    for (const auto value : cpu->gpr) std::fprintf(out, " %08X", value);
    std::fprintf(out, "\n fpr");
    for (const auto value : cpu->fpr) std::fprintf(out, " %016llX", static_cast<unsigned long long>(value.raw));
    std::fprintf(out, "\n");
    for (unsigned index = 0; index < 5; ++index) {
        const auto address = call.addresses[index];
        // RAM only; inspecting an MMIO register could acknowledge an event.
        if (!((address >= 0x80000000u && address < 0x81800000u) ||
              (address >= 0x90000000u && address < 0x98000000u))) continue;
        if (!Memory::Contains(address, 128)) continue;
        const auto* bytes = Memory::GetPointer(address, 128);
        std::fprintf(out, " memr%u=%08X", index + 3, address);
        for (unsigned i = 0; i < 128; ++i) std::fprintf(out, "%s%02X", i % 4 == 0 ? " " : "", bytes[i]);
        std::fprintf(out, "\n");
        if (call.id == 0 && std::strcmp(phase, "enter") == 0) {
            for (unsigned field = 0; field < 32; field += 4) {
                const auto pointer = (uint32_t(bytes[field]) << 24) | (uint32_t(bytes[field+1]) << 16) |
                    (uint32_t(bytes[field+2]) << 8) | bytes[field+3];
                if (!((pointer >= 0x80000000u && pointer < 0x81800000u) ||
                      (pointer >= 0x90000000u && pointer < 0x98000000u)) || !Memory::Contains(pointer, 160)) continue;
                const auto* child = Memory::GetPointer(pointer, 160);
                std::fprintf(out, " childr%u+%02X=%08X", index+3, field, pointer);
                for (unsigned i = 0; i < 160; ++i) std::fprintf(out, "%s%02X", i % 4 == 0 ? " " : "", child[i]);
                std::fprintf(out, "\n");
            }
        }
    }
}

void TraceEnter(const void* scope, const CpuContext* cpu, const ResolvedTarget& resolved) {
    auto& trace = g_moduleTrace;
    if (trace.output == nullptr && (++trace.poll & 4095u) == 0) {
        if (auto* request = std::fopen("dynamic_module_capture.request", "r")) {
            unsigned count = 0;
            unsigned startOffset = 0;
            const bool valid = std::fscanf(request, "%127s %u %x", trace.name, &count, &startOffset) >= 2;
            std::fclose(request);
            std::remove("dynamic_module_capture.request");
            if (valid && count > 0 && count <= 4096) {
                trace.output = std::fopen("dynamic_module_capture.txt", "w");
                trace.remaining = count;
                trace.nextId = 0;
                trace.startOffset = startOffset;
                trace.started = startOffset == 0;
                if (trace.output) std::fprintf(trace.output, "module=%s calls=%u\n", trace.name, count);
            }
        }
    }
    if (trace.output == nullptr || trace.remaining == 0 || resolved.module->debugName == nullptr ||
        std::strcmp(trace.name, resolved.module->debugName) != 0) return;
    if (!trace.started) {
        if (resolved.function->offset != trace.startOffset) return;
        trace.started = true;
    }
    TraceCall call{};
    call.id = trace.nextId++;
    for (unsigned i = 0; i < 5; ++i) call.addresses[i] = cpu->gpr[i + 3];
    --trace.remaining;
    trace.calls.emplace(scope, call);
    WriteTraceState("enter", call, cpu, resolved.function->offset);
}

void TraceLeave(const void* scope, const CpuContext* cpu) {
    auto& trace = g_moduleTrace;
    if (trace.output == nullptr) return;
    const auto it = trace.calls.find(scope);
    if (it == trace.calls.end()) return;
    WriteTraceState("leave", it->second, cpu, 0);
    trace.calls.erase(it);
    if (trace.remaining == 0 && trace.calls.empty()) {
        std::fclose(trace.output);
        trace.output = nullptr;
    }
}
#endif

bool ValidateInstance(const TemplateRecord* module, uint32_t base, InstanceLayout* layout) {
    const auto validator = module->validateInstance != nullptr ? module->validateInstance : module->resolveInstance;
    return validator(base, layout);
}

bool ValidateDispatchInstance(const TemplateRecord* module, uint32_t base, InstanceLayout* layout) {
    const auto validator = module->validateDispatch != nullptr
        ? module->validateDispatch
        : (module->validateInstance != nullptr ? module->validateInstance : module->resolveInstance);
    return validator(base, layout);
}

bool ValidateTemplate(const TemplateRecord* moduleTemplate) {
    if (moduleTemplate == nullptr || moduleTemplate->canonicalBase == 0 ||
        moduleTemplate->imageSize == 0 || moduleTemplate->functions == nullptr ||
        moduleTemplate->functionCount == 0 || moduleTemplate->resolveInstance == nullptr) {
        return false;
    }
    if ((moduleTemplate->bssSize == 0) != (moduleTemplate->canonicalBssBase == 0)) {
        return false;
    }
    const uint64_t canonicalImageEnd =
        static_cast<uint64_t>(moduleTemplate->canonicalBase) + moduleTemplate->imageSize;
    if (canonicalImageEnd >= 0x100000000ull) {
        return false;
    }
    if (moduleTemplate->bssSize != 0) {
        const uint64_t canonicalBssEnd =
            static_cast<uint64_t>(moduleTemplate->canonicalBssBase) + moduleTemplate->bssSize;
        if (canonicalBssEnd >= 0x100000000ull ||
            RangesOverlap(moduleTemplate->canonicalBase, moduleTemplate->imageSize,
                          moduleTemplate->canonicalBssBase, moduleTemplate->bssSize)) {
            return false;
        }
    }
    for (size_t i = 0; i < moduleTemplate->functionCount; ++i) {
        const auto& function = moduleTemplate->functions[i];
        if (function.entry == nullptr || function.offset >= moduleTemplate->imageSize) {
            return false;
        }
        if (i != 0 && moduleTemplate->functions[i - 1].offset >= function.offset) {
            return false;
        }
    }
    return true;
}

bool IsRegisteredTemplate(const TemplateRecord* moduleTemplate) {
    const auto& templates = Templates();
    return std::find(templates.begin(), templates.end(), moduleTemplate) != templates.end();
}

bool RangesOverlap(uint32_t aBase, uint32_t aSize, uint32_t bBase, uint32_t bSize) {
    const uint64_t aEnd = static_cast<uint64_t>(aBase) + aSize;
    const uint64_t bEnd = static_cast<uint64_t>(bBase) + bSize;
    return static_cast<uint64_t>(aBase) < bEnd && static_cast<uint64_t>(bBase) < aEnd;
}

const FunctionRecord* FindFunction(const TemplateRecord* moduleTemplate, uint32_t offset) {
    const auto* begin = moduleTemplate->functions;
    const auto* end = begin + moduleTemplate->functionCount;
    const auto* it = std::lower_bound(begin, end, offset,
        [](const FunctionRecord& function, uint32_t targetOffset) {
            return function.offset < targetOffset;
        });
    return it != end && it->offset == offset ? it : nullptr;
}

bool AttachInstanceLocked(
    const TemplateRecord* moduleTemplate,
    uint32_t runtimeBase,
    bool replaceSameBase,
    InstanceToken& token) {
    token = {};
    InstanceLayout layout{};
    if (!IsRegisteredTemplate(moduleTemplate) || !ValidateInstance(moduleTemplate, runtimeBase, &layout)) {
        return false;
    }
    if (layout.imageBase != runtimeBase) {
        return false;
    }
    if (moduleTemplate->bssSize != 0 && layout.bssBase == 0) {
        return false;
    }
    const uint64_t end = static_cast<uint64_t>(runtimeBase) + moduleTemplate->imageSize;
    if (end >= 0x100000000ull) {
        return false;
    }
    if (moduleTemplate->bssSize != 0) {
        const uint64_t bssEnd = static_cast<uint64_t>(layout.bssBase) + moduleTemplate->bssSize;
        if (bssEnd >= 0x100000000ull ||
            RangesOverlap(runtimeBase, moduleTemplate->imageSize,
                          layout.bssBase, moduleTemplate->bssSize)) {
            return false;
        }
    }

    auto& instances = Instances();
    // Guest allocation reuse need not preserve the old image base. Discard
    // invalidated layouts before testing overlaps, even if no dispatch lookup
    // occurred between the overwrite and this code publication. A still-valid
    // instance must continue to reject conflicting image/BSS ranges below.
    for (auto it = instances.begin(); it != instances.end();) {
        const auto& existing = it->second;
        InstanceLayout currentLayout{};
        if (!ValidateInstance(existing.module, existing.runtimeBase, &currentLayout) ||
            currentLayout.imageBase != existing.runtimeBase ||
            currentLayout.bssBase != existing.runtimeBssBase) {
            it = instances.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [base, existing] : instances) {
        if (base == runtimeBase && replaceSameBase) {
            continue;
        }
        if (RangesOverlap(runtimeBase, moduleTemplate->imageSize,
                          base, existing.module->imageSize)) {
            return false;
        }
        if (existing.module->bssSize != 0 &&
            RangesOverlap(runtimeBase, moduleTemplate->imageSize,
                          existing.runtimeBssBase, existing.module->bssSize)) {
            return false;
        }
        if (moduleTemplate->bssSize != 0 &&
            (RangesOverlap(layout.bssBase, moduleTemplate->bssSize,
                           base, existing.module->imageSize) ||
             (existing.module->bssSize != 0 &&
              RangesOverlap(layout.bssBase, moduleTemplate->bssSize,
                            existing.runtimeBssBase, existing.module->bssSize)))) {
            return false;
        }
    }

    if (!replaceSameBase && instances.find(runtimeBase) != instances.end()) {
        return false;
    }
    const uint64_t generation = NextGeneration().fetch_add(1, std::memory_order_relaxed);
    instances[runtimeBase] = {moduleTemplate, runtimeBase, layout.bssBase, generation};
    token = {moduleTemplate, runtimeBase, generation};
    return true;
}

} // namespace

bool RegisterTemplate(const TemplateRecord* moduleTemplate) {
    if (!ValidateTemplate(moduleTemplate)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& templates = Templates();
    if (std::find(templates.begin(), templates.end(), moduleTemplate) == templates.end()) {
        templates.push_back(moduleTemplate);
    }
    return true;
}

bool AttachInstance(const TemplateRecord* moduleTemplate, uint32_t runtimeBase, InstanceToken& token) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    return AttachInstanceLocked(moduleTemplate, runtimeBase, false, token);
}

bool DetachInstance(const InstanceToken& token) {
    if (!token) {
        return false;
    }
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& instances = Instances();
    const auto it = instances.find(token.runtimeBase);
    if (it == instances.end() || it->second.module != token.module ||
        it->second.generation != token.generation) {
        return false;
    }
    instances.erase(it);
    return true;
}

bool NotifyCodePublication(uint32_t candidateBase) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const TemplateRecord* match = nullptr;
    InstanceLayout matchedLayout{};
    std::vector<uint32_t> ambiguousBases;
    for (const auto* moduleTemplate : Templates()) {
        InstanceLayout layout{};
        if (!moduleTemplate->resolveInstance(candidateBase, &layout)) {
            continue;
        }
        if (layout.imageBase == 0) {
            continue;
        }
        if (match != nullptr && (match != moduleTemplate || matchedLayout.imageBase != layout.imageBase)) {
            // Ambiguous signatures are never first-wins.
            ambiguousBases.push_back(matchedLayout.imageBase);
            ambiguousBases.push_back(layout.imageBase);
            for (const uint32_t base : ambiguousBases) {
                Instances().erase(base);
            }
            return false;
        }
        match = moduleTemplate;
        matchedLayout = layout;
    }
    if (match == nullptr) {
        return false;
    }

    InstanceToken ignored;
    return AttachInstanceLocked(match, matchedLayout.imageBase, true, ignored);
}

bool TryResolveTarget(uint32_t target, ResolvedTarget& resolved) {
    resolved = {};
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& instances = Instances();

    for (auto it = instances.begin(); it != instances.end();) {
        const auto instance = it->second;
        const uint64_t end =
            static_cast<uint64_t>(instance.runtimeBase) + instance.module->imageSize;
        // A target outside this instance cannot resolve through it. In particular,
        // static-DOL dispatch must not rehash every loaded RSO on every call.
        // Matching instances are still validated below, and publication prunes
        // all stale layouts before admitting overlapping heap reuse.
        if (target < instance.runtimeBase || static_cast<uint64_t>(target) >= end) {
            ++it;
            continue;
        }
        InstanceLayout layout{};
        if (!ValidateDispatchInstance(instance.module, instance.runtimeBase, &layout) ||
            layout.imageBase != instance.runtimeBase ||
            layout.bssBase != instance.runtimeBssBase) {
            it = instances.erase(it);
            continue;
        }
        if (target >= instance.runtimeBase && static_cast<uint64_t>(target) < end) {
            const uint32_t offset = target - instance.runtimeBase;
            const auto* function = FindFunction(instance.module, offset);
            if (function != nullptr) {
                resolved = {instance.module, function, instance.runtimeBase, instance.runtimeBssBase, instance.generation};
                return true;
            }
        }
        ++it;
    }
    return false;
}

bool TryResolveCurrentFunction(const CpuContext* cpu, uint32_t functionOffset, ResolvedTarget& resolved) {
    resolved = {};
    if (cpu == nullptr || cpu->dynamicModuleTemplate == nullptr || cpu->dynamicModuleGeneration == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto* moduleTemplate = static_cast<const TemplateRecord*>(cpu->dynamicModuleTemplate);
    const auto instance = Instances().find(cpu->dynamicModuleRuntimeBase);
    if (instance == Instances().end() || instance->second.module != moduleTemplate ||
        instance->second.generation != cpu->dynamicModuleGeneration ||
        cpu->dynamicModuleRuntimeBssBase != instance->second.runtimeBssBase) {
        return false;
    }
    InstanceLayout layout{};
    if (!ValidateDispatchInstance(moduleTemplate, cpu->dynamicModuleRuntimeBase, &layout) ||
        layout.imageBase != cpu->dynamicModuleRuntimeBase ||
        layout.bssBase != cpu->dynamicModuleRuntimeBssBase) {
        return false;
    }
    const auto* function = FindFunction(moduleTemplate, functionOffset);
    if (function == nullptr) {
        return false;
    }
    resolved = {
        moduleTemplate,
        function,
        cpu->dynamicModuleRuntimeBase,
        cpu->dynamicModuleRuntimeBssBase,
        cpu->dynamicModuleGeneration,
    };
    return true;
}

bool CanDispatchTarget(uint32_t target) {
    ResolvedTarget ignored;
    return TryResolveTarget(target, ignored);
}

ScopedExecution::ScopedExecution(CpuContext* cpu, const ResolvedTarget& resolved) noexcept
    : cpu_(cpu) {
    if (cpu_ == nullptr || !resolved) {
        cpu_ = nullptr;
        return;
    }
    previousTemplate_ = cpu_->dynamicModuleTemplate;
    previousRuntimeBase_ = cpu_->dynamicModuleRuntimeBase;
    previousCanonicalBase_ = cpu_->dynamicModuleCanonicalBase;
    previousImageSize_ = cpu_->dynamicModuleImageSize;
    previousRuntimeBssBase_ = cpu_->dynamicModuleRuntimeBssBase;
    previousCanonicalBssBase_ = cpu_->dynamicModuleCanonicalBssBase;
    previousBssSize_ = cpu_->dynamicModuleBssSize;
    previousGeneration_ = cpu_->dynamicModuleGeneration;

    cpu_->dynamicModuleTemplate = resolved.module;
    cpu_->dynamicModuleRuntimeBase = resolved.runtimeBase;
    cpu_->dynamicModuleCanonicalBase = resolved.module->canonicalBase;
    cpu_->dynamicModuleImageSize = resolved.module->imageSize;
    cpu_->dynamicModuleRuntimeBssBase = resolved.runtimeBssBase;
    cpu_->dynamicModuleCanonicalBssBase = resolved.module->canonicalBssBase;
    cpu_->dynamicModuleBssSize = resolved.module->bssSize;
    cpu_->dynamicModuleGeneration = resolved.generation;
#if defined(MKW_GENERIC_DOL_BOOT)
    TraceEnter(this, cpu_, resolved);
#endif
}

ScopedExecution::~ScopedExecution() noexcept {
    if (cpu_ == nullptr) {
        return;
    }
#if defined(MKW_GENERIC_DOL_BOOT)
    TraceLeave(this, cpu_);
#endif
    cpu_->dynamicModuleTemplate = previousTemplate_;
    cpu_->dynamicModuleRuntimeBase = previousRuntimeBase_;
    cpu_->dynamicModuleCanonicalBase = previousCanonicalBase_;
    cpu_->dynamicModuleImageSize = previousImageSize_;
    cpu_->dynamicModuleRuntimeBssBase = previousRuntimeBssBase_;
    cpu_->dynamicModuleCanonicalBssBase = previousCanonicalBssBase_;
    cpu_->dynamicModuleBssSize = previousBssSize_;
    cpu_->dynamicModuleGeneration = previousGeneration_;
}

void ResetRegistryForTests() {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Templates().clear();
    Instances().clear();
    NextGeneration().store(1, std::memory_order_relaxed);
}

} // namespace DynamicModule

extern "C" void PPC_Icbi(uint32_t address) {
    DynamicModule::NotifyCodePublication(address & ~31u);
}
