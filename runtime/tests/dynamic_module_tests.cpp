#if defined(NDEBUG)
#error "Dynamic module tests require enabled assertions in every build configuration"
#endif

#include "dynamic_module.h"

#include <cassert>
#include <cstdint>
#include <unordered_set>

namespace {

std::unordered_set<uint32_t> g_templateABases;
std::unordered_set<uint32_t> g_templateBBases;
uint32_t g_resolveACalls = 0;
uint32_t g_publicationCalls = 0;
uint32_t g_dispatchACalls = 0;

bool ResolveA(uint32_t base, DynamicModule::InstanceLayout* layout) {
    ++g_resolveACalls;
    if (g_templateABases.count(base) == 0) {
        return false;
    }
    if (layout != nullptr) {
        layout->imageBase = base;
        layout->bssBase = base + 0x2000u;
    }
    return true;
}

bool ResolveADispatch(uint32_t base, DynamicModule::InstanceLayout* layout) {
    ++g_dispatchACalls;
    if (g_templateABases.count(base) == 0) {
        return false;
    }
    if (layout != nullptr) {
        layout->imageBase = base;
        layout->bssBase = base + 0x2000u;
    }
    return true;
}

bool ResolveB(uint32_t base, DynamicModule::InstanceLayout* layout) {
    if (g_templateBBases.count(base) == 0) {
        return false;
    }
    if (layout != nullptr) {
        layout->imageBase = base;
        layout->bssBase = base + 0x3000u;
    }
    return true;
}

bool ResolveAFromImageOrCodePublication(uint32_t probe, DynamicModule::InstanceLayout* layout) {
    ++g_publicationCalls;
    uint32_t base = probe;
    if (probe >= 0x120u && g_templateABases.find(probe) == g_templateABases.end()) {
        const uint32_t candidate = probe - 0x120u;
        if (g_templateABases.find(candidate) != g_templateABases.end()) {
            base = candidate;
        }
    }
    if (g_templateABases.find(base) == g_templateABases.end()) {
        return false;
    }
    if (layout != nullptr) {
        layout->imageBase = base;
        layout->bssBase = base + 0x2000u;
    }
    return true;
}

void Entry0(CpuContext*) {}
void Entry20(CpuContext*) {}

DynamicModule::FunctionRecord kFunctionsA[] = {
    {0x20u, &Entry20, true, 0u},
};

DynamicModule::FunctionRecord kFunctionsB[] = {
    {0x20u, &Entry20, true, 0u},
};

DynamicModule::TemplateRecord MakeTemplate(
    DynamicModule::FunctionRecord* functions,
    size_t count,
    DynamicModule::InstanceResolver resolver,
    uint8_t signatureByte,
    const char* name) {
    DynamicModule::TemplateRecord result{};
    result.preLinkSha256[0] = signatureByte;
    result.canonicalBase = 0x70000000u;
    result.imageSize = 0x1000u;
    result.canonicalBssBase = 0x71000000u;
    result.bssSize = 0x100u;
    result.functions = functions;
    result.functionCount = count;
    result.resolveInstance = resolver;
    result.debugName = name;
    return result;
}

void Reset() {
    DynamicModule::ResetRegistryForTests();
    g_templateABases.clear();
    g_templateBBases.clear();
    g_resolveACalls = 0;
    g_publicationCalls = 0;
    g_dispatchACalls = 0;
}

void TestExactBaseValidationPreservesPublicationAndStaleRejection() {
    Reset();
    auto module = MakeTemplate(kFunctionsA, 1, &ResolveAFromImageOrCodePublication, 0xBC, "exact-validation");
    module.validateInstance = &ResolveA;
    module.validateDispatch = &ResolveADispatch;
    assert(DynamicModule::RegisterTemplate(&module));
    g_templateABases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000120u));
    assert(g_publicationCalls == 1);
    const auto validationCalls = g_resolveACalls;
    DynamicModule::ResolvedTarget target{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, target));
    assert(g_publicationCalls == 1);
    assert(g_resolveACalls == validationCalls);
    assert(g_dispatchACalls == 1);
    CpuContext cpu{};
    {
        DynamicModule::ScopedExecution scope(&cpu, target);
        assert(DynamicModule::TryResolveCurrentFunction(&cpu, 0x20u, target));
        assert(g_publicationCalls == 1);
        assert(g_resolveACalls == validationCalls);
        assert(g_dispatchACalls == 2);
        g_templateABases.clear();
        assert(!DynamicModule::TryResolveCurrentFunction(&cpu, 0x20u, target));
        assert(g_dispatchACalls == 3);
        assert(!DynamicModule::TryResolveTarget(0x81000020u, target));
        assert(g_dispatchACalls == 4);
    }
    assert(g_publicationCalls == 1);
    g_templateABases.insert(0x81000000u);
    assert(!DynamicModule::TryResolveTarget(0x81000020u, target)); // No stale reattachment.
    assert(DynamicModule::NotifyCodePublication(0x81000120u));
    assert(g_publicationCalls == 2);
    assert(DynamicModule::TryResolveTarget(0x81000020u, target));
    assert(g_dispatchACalls == 5);
}

void TestUnrelatedTargetsDoNotRevalidateEveryImage() {
    Reset();
    auto module = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xAC, "bounded-resolve");
    assert(DynamicModule::RegisterTemplate(&module));
    g_templateABases.insert(0x81000000u);
    g_templateABases.insert(0x81200000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    assert(DynamicModule::NotifyCodePublication(0x81200000u));
    g_resolveACalls = 0;
    DynamicModule::ResolvedTarget target{};
    assert(!DynamicModule::TryResolveTarget(0x80004000u, target));
    assert(!DynamicModule::TryResolveTarget(0x81001000u, target));
    assert(g_resolveACalls == 0);
    assert(DynamicModule::TryResolveTarget(0x81200020u, target));
    assert(g_resolveACalls == 1);
    g_templateABases.erase(0x81200000u);
    assert(!DynamicModule::TryResolveTarget(0x81200020u, target));
    assert(g_resolveACalls == 2);
}

void TestSameTemplateAtTwoBases() {
    Reset();
    auto module = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA1, "A");
    assert(DynamicModule::RegisterTemplate(&module));
    g_templateABases.insert(0x81000000u);
    g_templateABases.insert(0x81200000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    assert(DynamicModule::NotifyCodePublication(0x81200000u));

    DynamicModule::ResolvedTarget first{};
    DynamicModule::ResolvedTarget second{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, first));
    assert(DynamicModule::TryResolveTarget(0x81200020u, second));
    assert(first.module == &module && second.module == &module);
    assert(first.function == &kFunctionsA[0] && second.function == &kFunctionsA[0]);
    assert(first.runtimeBase == 0x81000000u);
    assert(second.runtimeBase == 0x81200000u);
}

void TestNestedExecutionRestoresParent() {
    Reset();
    auto moduleA = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA2, "A");
    auto moduleB = MakeTemplate(kFunctionsB, 1, &ResolveB, 0xB2, "B");
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));
    g_templateABases.insert(0x81000000u);
    g_templateBBases.insert(0x82000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    assert(DynamicModule::NotifyCodePublication(0x82000000u));

    DynamicModule::ResolvedTarget a{};
    DynamicModule::ResolvedTarget b{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, a));
    assert(DynamicModule::TryResolveTarget(0x82000020u, b));

    CpuContext cpu{};
    {
        DynamicModule::ScopedExecution outer(&cpu, a);
        assert(cpu.dynamicModuleTemplate == &moduleA);
        assert(cpu.dynamicModuleRuntimeBase == 0x81000000u);
        assert(DynamicModule::RebaseCanonicalAddress(&cpu, 0x70000080u) == 0x81000080u);
        assert(DynamicModule::RebaseCanonicalAddress(&cpu, 0x71000040u) == 0x81002040u);
        assert(DynamicModule::RebaseCanonicalAddress(&cpu, 0x80000080u) == 0x80000080u);
        {
            DynamicModule::ScopedExecution inner(&cpu, b);
            assert(cpu.dynamicModuleTemplate == &moduleB);
            assert(cpu.dynamicModuleRuntimeBase == 0x82000000u);
            assert(DynamicModule::RuntimeAddressFromOffset(&cpu, 0x44u) == 0x82000044u);
        }
        assert(cpu.dynamicModuleTemplate == &moduleA);
        assert(cpu.dynamicModuleRuntimeBase == 0x81000000u);
    }
    assert(cpu.dynamicModuleTemplate == nullptr);
    assert(cpu.dynamicModuleRuntimeBase == 0);
}

void TestCpuContextCopyIsFiberSafeState() {
    Reset();
    auto module = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA3, "A");
    assert(DynamicModule::RegisterTemplate(&module));
    g_templateABases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));

    DynamicModule::ResolvedTarget resolved{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, resolved));
    CpuContext fiberA{};
    CpuContext fiberB{};
    {
        DynamicModule::ScopedExecution active(&fiberA, resolved);
        fiberB = CpuContext{};
        assert(fiberA.dynamicModuleTemplate == &module);
        assert(fiberB.dynamicModuleTemplate == nullptr);
        CpuContext savedA = fiberA;
        assert(savedA.dynamicModuleRuntimeBase == 0x81000000u);
        assert(savedA.dynamicModuleGeneration == resolved.generation);
    }
}

void TestSignatureRejectionAndAmbiguityFailClosed() {
    Reset();
    auto moduleA = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA4, "A");
    auto moduleB = MakeTemplate(kFunctionsB, 1, &ResolveB, 0xB4, "B");
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));

    DynamicModule::ResolvedTarget resolved{};
    assert(!DynamicModule::TryResolveTarget(0x81000020u, resolved));

    g_templateABases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    assert(DynamicModule::TryResolveTarget(0x81000020u, resolved));
    g_templateBBases.insert(0x81000000u);
    DynamicModule::ResetRegistryForTests();
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));
    assert(!DynamicModule::NotifyCodePublication(0x81000000u));
    assert(!DynamicModule::TryResolveTarget(0x81000020u, resolved));
}

void TestHeapReuseInvalidatesCacheGeneration() {
    Reset();
    auto moduleA = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA5, "A");
    auto moduleB = MakeTemplate(kFunctionsB, 1, &ResolveB, 0xB5, "B");
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));

    g_templateABases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    DynamicModule::ResolvedTarget first{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, first));
    const uint64_t firstGeneration = first.generation;

    g_templateABases.clear();
    g_templateBBases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    DynamicModule::ResolvedTarget second{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, second));
    assert(second.module == &moduleB);
    assert(second.generation != firstGeneration);
}

void TestDetachTokenRejectsAba() {
    Reset();
    auto module = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA7, "A");
    assert(DynamicModule::RegisterTemplate(&module));
    g_templateABases.insert(0x81000000u);

    DynamicModule::InstanceToken first{};
    assert(DynamicModule::AttachInstance(&module, 0x81000000u, first));
    assert(DynamicModule::DetachInstance(first));

    DynamicModule::InstanceToken second{};
    assert(DynamicModule::AttachInstance(&module, 0x81000000u, second));
    assert(second.generation != first.generation);
    assert(!DynamicModule::DetachInstance(first));
    assert(DynamicModule::DetachInstance(second));
}

void TestMalformedTemplateRejected() {
    Reset();
    DynamicModule::FunctionRecord malformed[] = {
        {0x30u, &Entry0, true, 0u},
        {0x20u, &Entry20, true, 0u},
    };
    auto module = MakeTemplate(malformed, 2, &ResolveA, 0xA6, "bad");
    assert(!DynamicModule::RegisterTemplate(&module));

    auto overlap = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA8, "canonical-overlap");
    overlap.canonicalBssBase = overlap.canonicalBase + 0x800u;
    assert(!DynamicModule::RegisterTemplate(&overlap));

    auto overflow = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xA9, "canonical-overflow");
    overflow.canonicalBase = 0xFFFFF800u;
    assert(!DynamicModule::RegisterTemplate(&overflow));
}

void TestRuntimeBssOverlapRejected() {
    Reset();
    auto moduleA = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xAA, "A");
    auto moduleB = MakeTemplate(kFunctionsB, 1, &ResolveB, 0xAB, "B");
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));

    // A uses image [81000000,81001000) and BSS [81002000,81002100).
    g_templateABases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));

    // B at 80FFF000 would place BSS at 81002000, colliding with A's BSS.
    g_templateBBases.insert(0x80FFF000u);
    assert(!DynamicModule::NotifyCodePublication(0x80FFF000u));

    DynamicModule::ResolvedTarget unresolved{};
    assert(!DynamicModule::TryResolveTarget(0x80FFF020u, unresolved));
}

void TestCodeSectionPublicationReturnsExactImageBase() {
    Reset();
    auto module = MakeTemplate(kFunctionsA, 1, &ResolveAFromImageOrCodePublication, 0xAC, "section-publish");
    assert(DynamicModule::RegisterTemplate(&module));
    g_templateABases.insert(0x81000000u);

    assert(DynamicModule::NotifyCodePublication(0x81000120u));
    DynamicModule::ResolvedTarget resolved{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, resolved));
    assert(resolved.runtimeBase == 0x81000000u);
    assert(resolved.runtimeBssBase == 0x81002000u);
}

void TestOverlappingHeapReusePrunesStaleInstanceBeforePublication() {
    Reset();
    auto moduleA = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xAD, "old-allocation");
    auto moduleB = MakeTemplate(kFunctionsB, 1, &ResolveB, 0xAE, "reused-allocation");
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));
    g_templateABases.insert(0x81000000u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    DynamicModule::ResolvedTarget oldTarget{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, oldTarget));

    // Reuse need not preserve the old allocation's exact base. No dispatch
    // probe occurs between the free/overwrite and the new code publication.
    g_templateABases.clear();
    g_templateBBases.insert(0x81000080u);
    assert(DynamicModule::NotifyCodePublication(0x81000080u));
    DynamicModule::ResolvedTarget newTarget{};
    assert(DynamicModule::TryResolveTarget(0x810000A0u, newTarget));
    assert(newTarget.module == &moduleB);
    assert(newTarget.generation != oldTarget.generation);
    assert(!DynamicModule::TryResolveTarget(0x81000020u, oldTarget));
}

void TestOverlappingLiveInstanceStillRejectsPublication() {
    Reset();
    auto moduleA = MakeTemplate(kFunctionsA, 1, &ResolveA, 0xAF, "live-allocation");
    auto moduleB = MakeTemplate(kFunctionsB, 1, &ResolveB, 0xB0, "overlap");
    assert(DynamicModule::RegisterTemplate(&moduleA));
    assert(DynamicModule::RegisterTemplate(&moduleB));
    g_templateABases.insert(0x81000000u);
    g_templateBBases.insert(0x81000080u);
    assert(DynamicModule::NotifyCodePublication(0x81000000u));
    assert(!DynamicModule::NotifyCodePublication(0x81000080u));
    DynamicModule::ResolvedTarget target{};
    assert(DynamicModule::TryResolveTarget(0x81000020u, target));
    assert(target.module == &moduleA);
    assert(!DynamicModule::TryResolveTarget(0x810000A0u, target));
}

} // namespace

int main() {
    TestExactBaseValidationPreservesPublicationAndStaleRejection();
    TestUnrelatedTargetsDoNotRevalidateEveryImage();
    TestSameTemplateAtTwoBases();
    TestNestedExecutionRestoresParent();
    TestCpuContextCopyIsFiberSafeState();
    TestSignatureRejectionAndAmbiguityFailClosed();
    TestHeapReuseInvalidatesCacheGeneration();
    TestDetachTokenRejectsAba();
    TestMalformedTemplateRejected();
    TestRuntimeBssOverlapRejected();
    TestCodeSectionPublicationReturnsExactImageBase();
    TestOverlappingHeapReusePrunesStaleInstanceBeforePublication();
    TestOverlappingLiveInstanceStillRejectsPublication();
    return 0;
}
