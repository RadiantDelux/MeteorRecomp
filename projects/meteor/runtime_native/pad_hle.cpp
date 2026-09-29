#include <cstdint>

#include "abi_bridge.h"

// Shared host-backed PADRead implementation.  RDSPAF links the same SDK entry
// at 0x802211DC instead of the MKW address used by the generic runtime.
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr);

namespace {

extern "C" uint32_t Meteor_PADRead(uint32_t statusPtr)
{
    return PAD__Read_HLE(statusPtr);
}

} // namespace

// HEADLESS validation of RDSPAF 0x802211DC shows the retail PADRead ABI:
// one PADStatus[4] output pointer and a u32 rumble/reset mask return value.
// Reusing the shared implementation keeps Aurora's real GameCube-pad sampling
// while also applying WiiRemoteInput::HideRemotesFromPad(), so a port served
// through KPAD is reported as PAD_ERR_NO_CONTROLLER instead of shadowing Wii
// input as an SI pad stuck in PAD_ERR_NOT_READY.
REGISTER_TITLE_NATIVE_FUNCTION(0x802211DC, Meteor_PADRead);
