#pragma once

#include <cstdint>

struct CpuContext;

namespace AxDspHle {

void SetGuestStateLayout(uint32_t initializedFlag,
                         uint32_t assertPending,
                         uint32_t assertTask,
                         uint32_t reservedState,
                         uint32_t currentTask,
                         uint32_t firstTask,
                         uint32_t runningTask);
void Init();
// Prepare only host-side AX resources when a title keeps the retail DSPInit
// translated.  This must not reset guest DSP globals or synthesize protocol mail.
void PrepareHostMixState();
void InitForAXOut(CpuContext* ctx);
void Stop();

uint32_t CheckInit();
uint32_t AddTask(uint32_t taskPtr);
void SendMailToDSP(uint32_t mail);
uint32_t CheckMailToDSP();
uint32_t CheckMailFromDSP();
uint32_t ReadMailFromDSP();
uint32_t AssertTask(uint32_t taskPtr);
void ServiceDeferredCallbacks();

// INVARIANT: no guest code may observe mix output before this returns. Called from
// Audio_HLE_Tick before the AI DMA block reaches the backend and before the AI callback
// runs __AXOutNewFrame, the two places guest code reads the mix's PB/output/aux data.
// Cheap when the mix already finished (the normal case).
void JoinMixWorker();

// Joins and tears down the mix worker thread. Call before the guest memory map
// is destroyed or re-created.
void ShutdownMixWorker();

void SetMixWorkerEnabled(bool enabled);

void InitAram();

}
