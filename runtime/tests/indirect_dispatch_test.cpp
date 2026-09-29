#include "abi_bridge.h"
#include <cassert>

void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept { std::abort(); }

namespace {
void Translated(CpuContext* cpu) { cpu->gpr[3] = 1; }
void Native(CpuContext* cpu) { cpu->gpr[3] = 2; }
}

int main() {
    // A lookup before publication must not poison the memoized result.
    assert(!TranslatedFunctionRegistry::FindRawByAddressPtr(0x80001000));
    const BulkTranslatedFunctionRecord registrations[] = {
        {0x80001000, "synthetic guest", Translated, FunctionKind::BaseTranslated, true, 0, 0, 0, true},
        {0x80001004, "unmodified guest", Translated, FunctionKind::BaseTranslated, true, 0, 0, 0, true},
        {0x80001000, "native replacement", Native, FunctionKind::Native, false, kPpcAllNonvolatileFprMask, 0, 0, true},
        {0x80001008, "native only", Native, FunctionKind::Native, false, kPpcAllNonvolatileFprMask, 0, 0, true},
    };
    RegisterBulkTranslatedFunctions(registrations, std::size(registrations));
    const RawDispatchRecord entries[] = {
        {0x80001000, Translated, 0, false}, {0x80001004, Translated, 0, false},
    };
    const StaticIndirectDispatchPage page{0, 2, 0};
    std::array<StaticIndirectDispatchSegment, 256> segments{};
    segments[0x80] = {&page, 1, 1};
    const StaticIndirectDispatchTable table{"synthetic", segments.data(), entries, std::size(entries)};
    RegisterStaticIndirectDispatchTable(&table);
    TranslatedFunctionRegistry::Finalize();

    CpuContext cpu{};
    for (int repeat = 0; repeat != 2; ++repeat) {
        const auto* replacement = TranslatedFunctionRegistry::FindRawByAddressPtr(0x80001000);
        assert(replacement && replacement->entry == Native);
        assert(replacement->preserveNonvolatileGprs);
        assert(replacement->nonvolatileFprWriteMask == kPpcAllNonvolatileFprMask);
        replacement->entry(&cpu);
        assert(cpu.gpr[3] == 2);
        assert(TranslatedFunctionRegistry::FindByAddressPtr(0x80001000)->rawCpuInvoker == Native);
        assert(TranslatedFunctionRegistry::FindRawByAddressPtr(0x80001004)->entry == Translated);
        assert(TranslatedFunctionRegistry::FindRawByAddressPtr(0x80001008)->entry == Native);
        assert(!TranslatedFunctionRegistry::FindRawByAddressPtr(0x8000100c));
    }
}
