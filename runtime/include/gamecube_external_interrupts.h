#pragma once

#include <cstdint>

struct CpuContext;

// Services guest-visible GameCube external interrupt sources that are modeled
// by the shared runtime but whose SDK handlers remain translated guest code.
// Call only from interrupt-safe translated ABI/backedge boundaries.
void GameCube_ServiceExternalInterrupts(uint32_t boundaryPc, CpuContext* ctx);
