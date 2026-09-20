#include "abi_bridge.h"
#include "runtime_log.h"

#include <cstdlib>

#if defined(MKW_GENERIC_DOL_BOOT)
namespace {
[[noreturn]] void UnexpectedMkwOnlyReference(const char* symbol) {
    RT_LOG(RT_TAG_RUNTIME) << "Generic DOL runtime reached MKW-only translated reference: "
                           << symbol << std::endl;
    std::abort();
}
}

// These symbols are referenced by hand-written MKW HLE translation units.  In
// generic-DOL mode those HLE registrations are disabled, but the object code is
// still linked because it also provides shared host services.  Keep link-time
// closure explicit and fail loudly if an MKW-only path is ever reached.
extern "C" void func_801D9E94(CpuContext*) { UnexpectedMkwOnlyReference("func_801D9E94"); }
extern "C" void func_801D8D30(CpuContext*) { UnexpectedMkwOnlyReference("func_801D8D30"); }
extern "C" void func_8012B830(CpuContext*) { UnexpectedMkwOnlyReference("func_8012B830"); }
extern "C" void func_801AADE0(CpuContext*) { UnexpectedMkwOnlyReference("func_801AADE0"); }
extern "C" void func_801A961C(CpuContext*) { UnexpectedMkwOnlyReference("func_801A961C"); }
extern "C" void func_8055531C(CpuContext*) { UnexpectedMkwOnlyReference("func_8055531C"); }
extern "C" void func_801A1ED8(CpuContext*) { UnexpectedMkwOnlyReference("func_801A1ED8"); }
#endif
