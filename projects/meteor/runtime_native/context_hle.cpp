#include "abi_bridge.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <atomic>

// The shared Wii runtime already has the non-returning OSLoadContext/rfi bridge.
// It is address-independent; the name reflects the SDK address in the original
// Mario Kart build where the helper was first implemented.
extern "C" [[noreturn]] void OS__LoadContext_801a1f58(CpuContext* ctx);
extern "C" void SelectThread_801a9c08(CpuContext* ctx);
extern "C" void OSCreateThread_HLE_801a9e84(CpuContext* ctx);
extern "C" void func_80209E24(CpuContext* ctx);
extern "C" void func_80210990(CpuContext* ctx);

void Meteor_ConfigureOsHleGuestState() noexcept;

namespace {
std::atomic<bool> g_meteorHasFiberThreads{false};

void LogSchedulerBridgeOnce(uint32_t bit, const char* name, uint32_t address)
{
    static std::atomic<uint32_t> loggedMask{0u};
    if ((loggedMask.fetch_or(bit, std::memory_order_relaxed) & bit) != 0u) {
        return;
    }
    RT_LOG(RT_TAG_OS) << "RDSPAF " << name << " 0x" << std::hex << address
                      << ": using shared fiber-aware Wii OS HLE" << std::dec << std::endl;
}
} // namespace

extern "C" void Meteor_OSLoadContext(CpuContext* cpu)
{
    Meteor_ConfigureOsHleGuestState();
    if (!g_meteorHasFiberThreads.load(std::memory_order_acquire)) {
        // Before OSCreateThread has established host fibers, retail scheduler
        // wakeups still need the translated SDK's ordinary C++ continuation.
        // Switching this call to the non-local rfi bridge too early would target
        // SelectThread's interior OSSaveContext return (0x80210A90) without a
        // host fiber to resume it.
        func_80209E24(cpu);
        return;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        RT_LOG(RT_TAG_OS)
            << "RDSPAF OSLoadContext 0x80209E24: using shared non-returning rfi bridge"
            << std::endl;
    }
    OS__LoadContext_801a1f58(cpu);
}

extern "C" void Meteor_SelectThread(CpuContext* cpu)
{
    Meteor_ConfigureOsHleGuestState();
    if (!g_meteorHasFiberThreads.load(std::memory_order_acquire)) {
        // SelectThread is already used by the SDK during early boot, before any
        // OSCreateThread call that can establish a host fiber. Keep that phase on
        // the translated retail scheduler; once the first fiber-backed thread is
        // created, later reschedules must use the host-fiber implementation.
        func_80210990(cpu);
        return;
    }

    LogSchedulerBridgeOnce(1u << 0, "SelectThread", 0x80210990u);
    SelectThread_801a9c08(cpu);
}

extern "C" void Meteor_OSCreateThread(CpuContext* cpu)
{
    Meteor_ConfigureOsHleGuestState();
    LogSchedulerBridgeOnce(1u << 1, "OSCreateThread", 0x80210BD0u);
    static std::atomic<uint32_t> createTraceCount{0u};
    const uint32_t traceIndex = createTraceCount.fetch_add(1u, std::memory_order_relaxed);
    if (cpu && traceIndex < 12u) {
        RT_LOG("meteor-fiber") << "create #" << traceIndex
                               << " thread=0x" << std::hex << cpu->gpr[3]
                               << " entry=0x" << cpu->gpr[4]
                               << " arg=0x" << cpu->gpr[5]
                               << " stack=0x" << cpu->gpr[6]
                               << " size=0x" << cpu->gpr[7]
                               << std::dec << " prio=" << static_cast<int32_t>(cpu->gpr[8])
                               << " attr=" << cpu->gpr[9] << std::endl;
    }
    OSCreateThread_HLE_801a9e84(cpu);
    if (cpu && cpu->gpr[3] != 0u) {
        g_meteorHasFiberThreads.store(true, std::memory_order_release);
    }
}

// Ghidra headless validates RDSPAF 0x80209E24 as the SDK OSLoadContext body.
// The statically translated version cannot model the terminal rfi: returning to
// its C++ caller makes that caller continue with the newly loaded thread's GPRs.
REGISTER_TITLE_NATIVE_FUNCTION(0x80209E24, Meteor_OSLoadContext);
REGISTER_TITLE_NATIVE_FUNCTION(0x80210990, Meteor_SelectThread);
REGISTER_TITLE_NATIVE_FUNCTION(0x80210BD0, Meteor_OSCreateThread);
