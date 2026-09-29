#pragma once

// Canonical guest-platform selection for the shared WiiCompiled runtime.
//
// CMake normally defines WIICOMPILED_GUEST_PLATFORM to one of the numeric IDs
// below. The legacy MKW_GAMECUBE_DOL_BOOT flag remains a supported compatibility
// input and takes precedence for translation units/tests that still define it
// directly. With neither definition present, Wii is the conservative default.
#define WIICOMPILED_GUEST_PLATFORM_WII 1
#define WIICOMPILED_GUEST_PLATFORM_GAMECUBE 2

#if defined(MKW_GAMECUBE_DOL_BOOT)
#define WIICOMPILED_GUEST_PLATFORM_ACTIVE WIICOMPILED_GUEST_PLATFORM_GAMECUBE
#elif defined(WIICOMPILED_GUEST_PLATFORM)
#define WIICOMPILED_GUEST_PLATFORM_ACTIVE WIICOMPILED_GUEST_PLATFORM
#else
#define WIICOMPILED_GUEST_PLATFORM_ACTIVE WIICOMPILED_GUEST_PLATFORM_WII
#endif

#if WIICOMPILED_GUEST_PLATFORM_ACTIVE != WIICOMPILED_GUEST_PLATFORM_WII && \
    WIICOMPILED_GUEST_PLATFORM_ACTIVE != WIICOMPILED_GUEST_PLATFORM_GAMECUBE
#error "Unsupported WIICOMPILED_GUEST_PLATFORM value"
#endif

#define WIICOMPILED_GUEST_IS_WII \
    (WIICOMPILED_GUEST_PLATFORM_ACTIVE == WIICOMPILED_GUEST_PLATFORM_WII)
#define WIICOMPILED_GUEST_IS_GAMECUBE \
    (WIICOMPILED_GUEST_PLATFORM_ACTIVE == WIICOMPILED_GUEST_PLATFORM_GAMECUBE)

namespace WiiCompiledTarget {

enum class GuestPlatform {
    Wii = WIICOMPILED_GUEST_PLATFORM_WII,
    GameCube = WIICOMPILED_GUEST_PLATFORM_GAMECUBE,
};

inline constexpr GuestPlatform kGuestPlatform =
    static_cast<GuestPlatform>(WIICOMPILED_GUEST_PLATFORM_ACTIVE);
inline constexpr bool kIsWii = WIICOMPILED_GUEST_IS_WII;
inline constexpr bool kIsGameCube = WIICOMPILED_GUEST_IS_GAMECUBE;

} // namespace WiiCompiledTarget
