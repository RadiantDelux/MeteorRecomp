cmake_minimum_required(VERSION 3.16)
project(mkw_recompiled)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WiiCompiled requires a 64-bit Clang toolchain")
endif()

if(WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(MKW_PLATFORM_WINDOWS TRUE)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64)$")
    # The first native macOS target is Apple Silicon. Intel and universal
    # binaries remain future compatibility work; do not silently claim them.
    set(MKW_PLATFORM_MACOS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|aarch64|arm64|ARM64)$")
    set(MKW_PLATFORM_LINUX TRUE)
else()
    message(FATAL_ERROR
        "WiiCompiled supports 64-bit LLVM-MinGW Clang on Windows, native Linux x86_64/aarch64, or Apple Clang on macOS arm64")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "WiiCompiled only supports Release builds")
endif()

option(WIICOMPILED_GRAPHICS_TESTS "Build Aurora FIFO regression tests" OFF)
option(WIICOMPILED_RUNTIME_COMPILE_AUDIT "Compile shared runtime without generated game products" OFF)
option(MKW_BUILD_PRODUCTS "Build translated WiiCompiled product targets" ON)
option(MKW_GENERIC_DOL_BOOT "Boot through the translated DOL entry point without MKW-specific constructor pre-runs" OFF)
option(MKW_GAMECUBE_DOL_BOOT "Use GameCube hardware/bootstrap defaults for a generic translated DOL" OFF)
set(WIICOMPILED_STORAGE_NAMESPACE "" CACHE STRING "Optional project/revision ID for isolated user data; empty preserves legacy saves")
set(WIICOMPILED_EXECUTABLE_BASENAME "WiiCompiled" CACHE STRING "Generic product filename without extension")
foreach(_name IN ITEMS WIICOMPILED_STORAGE_NAMESPACE WIICOMPILED_EXECUTABLE_BASENAME)
    if(NOT "${${_name}}" STREQUAL "" AND NOT "${${_name}}" MATCHES "^[A-Za-z0-9][A-Za-z0-9_-]*$")
        message(FATAL_ERROR "${_name} must be a single portable filename component")
    endif()
endforeach()
string(LENGTH "${WIICOMPILED_STORAGE_NAMESPACE}" _namespace_length)
if(_namespace_length GREATER 64 OR WIICOMPILED_STORAGE_NAMESPACE MATCHES "[A-Z]" OR
   WIICOMPILED_STORAGE_NAMESPACE MATCHES "^(con|prn|aux|nul|com[1-9]|lpt[1-9])$")
    message(FATAL_ERROR "WIICOMPILED_STORAGE_NAMESPACE must be a lowercase portable project ID (up to 64 characters)")
endif()
string(TOLOWER "${WIICOMPILED_EXECUTABLE_BASENAME}" _executable_lower)
if(_executable_lower STREQUAL "" OR _executable_lower MATCHES "^(con|prn|aux|nul|com[1-9]|lpt[1-9])$")
    message(FATAL_ERROR "WIICOMPILED_EXECUTABLE_BASENAME is not a valid portable executable name")
endif()
set(WIICOMPILED_GUEST_PLATFORM "auto" CACHE STRING
    "Guest hardware platform: auto, wii, or gamecube")
set_property(CACHE WIICOMPILED_GUEST_PLATFORM PROPERTY STRINGS auto wii gamecube)
string(TOLOWER "${WIICOMPILED_GUEST_PLATFORM}" WIICOMPILED_GUEST_PLATFORM_NORMALIZED)
if(NOT WIICOMPILED_GUEST_PLATFORM_NORMALIZED MATCHES "^(auto|wii|gamecube)$")
    message(FATAL_ERROR
        "WIICOMPILED_GUEST_PLATFORM must be one of: auto, wii, gamecube")
endif()

if(WIICOMPILED_GUEST_PLATFORM_NORMALIZED STREQUAL "auto")
    if(MKW_GAMECUBE_DOL_BOOT)
        set(WIICOMPILED_GUEST_PLATFORM_RESOLVED "gamecube")
    else()
        set(WIICOMPILED_GUEST_PLATFORM_RESOLVED "wii")
    endif()
else()
    set(WIICOMPILED_GUEST_PLATFORM_RESOLVED "${WIICOMPILED_GUEST_PLATFORM_NORMALIZED}")
endif()

if(WIICOMPILED_GUEST_PLATFORM_RESOLVED STREQUAL "gamecube")
    set(WIICOMPILED_GUEST_PLATFORM_ID 2)
else()
    set(WIICOMPILED_GUEST_PLATFORM_ID 1)
    if(MKW_GAMECUBE_DOL_BOOT)
        message(FATAL_ERROR
            "WIICOMPILED_GUEST_PLATFORM=wii conflicts with MKW_GAMECUBE_DOL_BOOT=ON")
    endif()
endif()
message(STATUS "WiiCompiled guest platform: ${WIICOMPILED_GUEST_PLATFORM_RESOLVED}")
set(MKW_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/../generated" CACHE PATH
    "Directory containing translator-generated RuntimeConfig/data/shard outputs")
set(MKW_TITLE_NATIVE_DIR "" CACHE PATH
    "Optional directory containing title-specific HLE/native registration sources")

# Preprocessor definitions that belong to this project's own code (the runtime,
# the translated shards and the product glue) and to nothing else. They are
# applied directory-scoped, immediately after the aurora add_subdirectory() call,
# so every target created afterwards (mkw_runtime_common, the translated shard
# libraries and the products in cmake/PublicProducts.cmake) inherits them while
# aurora-main and its third-party tree stay unaffected.
set(MKW_PROJECT_COMPILE_DEFINITIONS
    NOMINMAX
    WIICOMPILED_STORAGE_NAMESPACE="${WIICOMPILED_STORAGE_NAMESPACE}"
    WIICOMPILED_GUEST_PLATFORM=${WIICOMPILED_GUEST_PLATFORM_ID})
if(WIICOMPILED_GUEST_PLATFORM_RESOLVED STREQUAL "gamecube")
    list(APPEND MKW_PROJECT_COMPILE_DEFINITIONS MKW_GAMECUBE_DOL_BOOT=1)
endif()

set(CMAKE_CXX_STANDARD 17)

# Declared before the vendored libraries so the third-party block below can
# branch on it and so cmake/NativePrebuilt.cmake is included after the names it
# must provide are known.
set(MKW_NATIVE_PREBUILT_DIR "" CACHE PATH
    "Precompiled aurora/third-party package to link instead of building aurora-main")
# Maintainer-only counterpart: describe the configured aurora build so the
# package can be harvested from it.
set(MKW_NATIVE_PREBUILT_EXPORT_DIR "" CACHE PATH
    "Maintainer-only: write a description of the configured aurora build here")

# The two Crypto++ configuration macros are load-bearing for ABI: they shape the
# structs and inline code the headers generate, so a first-party TU including
# <cryptopp/...> must see exactly what the archive was compiled with. Both the
# from-source target below and the prebuilt import in cmake/NativePrebuilt.cmake
# consume this one definition.
set(MKW_CRYPTOPP_INTERFACE_DEFINITIONS CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)

# Riivolution XML is parsed with pugixml, matching the parser used by Dolphin.
# The four upstream files from v1.15 (ee86beb30e4973f5feffe3ce63bfa4fbadf72f38)
# are vendored so builds remain usable with FETCHCONTENT_FULLY_DISCONNECTED.
add_library(mkw_pugixml STATIC third_party/pugixml/pugixml.cpp)
add_library(mkw::pugixml ALIAS mkw_pugixml)
target_include_directories(mkw_pugixml PUBLIC third_party/pugixml)
target_compile_features(mkw_pugixml PUBLIC cxx_std_17)
set_target_properties(mkw_pugixml PROPERTIES UNITY_BUILD OFF)

# Linux guest-fiber scheduling (runtime/src/host_context.cpp) needs a symmetric
# stackful-coroutine primitive to stand in for Win32 Fibers. libco's co_switch() transfers
# directly to any other created coroutine, matching SwitchToFiber's semantics exactly (unlike
# asymmetric resume/yield coroutine libraries, which would need every call site restructured).
# Vendored from upstream (higan-emu/libco @ e18e09d, 2019-10-16, ISC license; valgrind.h is
# separately BSD-style licensed, see third_party/libco/LICENSE). Windows keeps native Fibers
# and macOS uses the project's x18-safe AArch64 assembly backend, so this target is Linux-only.
if(MKW_PLATFORM_LINUX)
    add_library(mkw_libco STATIC third_party/libco/libco.c)
    add_library(mkw::libco ALIAS mkw_libco)
    target_include_directories(mkw_libco PUBLIC third_party/libco)
    set_target_properties(mkw_libco PROPERTIES UNITY_BUILD OFF)
endif()

# Runtime configuration is real TOML, parsed by toml11 rather than a project-
# specific line parser. Keep it header-only and vendored so disconnected release
# builds have exactly the same parser as developer builds.
add_library(mkw_toml11 INTERFACE)
add_library(mkw::toml11 ALIAS mkw_toml11)
target_include_directories(mkw_toml11 SYSTEM INTERFACE third_party/toml11)
target_compile_features(mkw_toml11 INTERFACE cxx_std_17)

# Wii ES signatures use the standard binary-field sect233r1 curve. Crypto++
# supplies that curve, SHA-1, ECDSA and an OS-seeded CSPRNG. The complete 8.9.0
# source distribution is vendored. Assembly and algorithm-specific SIMD units
# are disabled to keep the library portable and independent of the runtime's
# x86-64-v3 policy; the static linker retains only the algorithms we reference.
#
# Crypto++ is by far the most expensive vendored library to compile (~150
# translation units), and under the shipped fixed toolchain it is bit-for-bit
# identical for every user, so the prebuilt package harvests its archive and
# this block is skipped entirely in that mode (see cmake/NativePrebuilt.cmake).
if(NOT MKW_NATIVE_PREBUILT_DIR)
    file(GLOB MKW_CRYPTOPP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/third_party/cryptopp/*.cpp")
    list(FILTER MKW_CRYPTOPP_SOURCES EXCLUDE REGEX
        "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
    list(FILTER MKW_CRYPTOPP_SOURCES EXCLUDE REGEX
        "(_avx|_ppc|_simd|_sse)\\.cpp$")
    add_library(mkw_cryptopp STATIC ${MKW_CRYPTOPP_SOURCES})
    add_library(mkw::cryptopp ALIAS mkw_cryptopp)
    target_include_directories(mkw_cryptopp SYSTEM PUBLIC third_party)
    target_compile_features(mkw_cryptopp PUBLIC cxx_std_17)
    target_compile_definitions(mkw_cryptopp PUBLIC ${MKW_CRYPTOPP_INTERFACE_DEFINITIONS})
    target_compile_options(mkw_cryptopp PRIVATE -w)
    set_target_properties(mkw_cryptopp PROPERTIES UNITY_BUILD OFF)
endif()

set(MKW_TRANSLATED_COMPILE_JOBS 0 CACHE STRING
    "Cap on concurrently compiling translated shard TUs via a Ninja job pool (0 = uncapped). \
Scheduling only - never affects output bytes, so it is deliberately outside the canonical flag fingerprint.")
set(MKW_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Optional self-contained C++/WinRT include directory")

# Precompiled aurora + third-party package (see launcher/Prepare-NativePrebuilt.ps1).
# Empty - the default - keeps the from-source behaviour every developer build
# uses. When set, aurora-main, its extern/ tree and the vendored Crypto++ are
# not compiled at all; imported targets with the same names are wired up from
# the package instead, so this project's own code compiles and links exactly
# as before.
set(MKW_AURORA_DIR "${CMAKE_CURRENT_LIST_DIR}/../aurora-main")

# The translator's explicit float casts are PPC single-precision rounding points.
# Fast-math may erase them and change guest-visible integer conversions.
set(MKW_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)

# ----------------------------------------------------------------------
# Third-party: aurora-main (provides SDL3 + GPU backends)
# ----------------------------------------------------------------------
if(MKW_NATIVE_PREBUILT_DIR)
    # Precompiled aurora + third-party archives. The source trees are still
    # present (the runtime includes aurora headers directly), they are simply
    # not compiled here.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuilt.cmake")
else()
    if(NOT EXISTS ${MKW_AURORA_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "Requested aurora-main but ${MKW_AURORA_DIR} is missing")
    endif()
    set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
    if(MKW_PLATFORM_WINDOWS)
        set(DAWN_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER ON CACHE BOOL "" FORCE)
    elseif(MKW_PLATFORM_MACOS)
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    else()
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    endif()
    set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # Provide a tiny stub for DXProgrammableCapture when the SDK/PIX headers are
    # missing (common on MinGW). Dawn only includes the header; no symbols are
    # referenced when PIX isn't present.
    if(MKW_PLATFORM_WINDOWS)
        set(MKW_DX_STUB_DIR "${CMAKE_BINARY_DIR}/aurora_dx_stubs")
        if(NOT EXISTS "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h")
            file(MAKE_DIRECTORY ${MKW_DX_STUB_DIR})
            file(WRITE "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h"
"#pragma once\n// Stubbed PIX capture header for Dawn; no functionality when PIX isn't present.\n")
        endif()
    endif()
    # Deliberately NOT injected project-wide. Only a from-source Dawn build ever
    # includes DXProgrammableCapture.h, and this tree consumes Dawn as a prebuilt
    # package (AURORA_DAWN_PROVIDER=package), so the old
    # include_directories()/add_compile_options(-I...) pair only added a dead
    # -I to every one of the ~500 third-party and runtime compile lines. The
    # aurora_* targets that do want it get it from the
    # target_include_directories() call further down; a from-source Dawn build
    # should add it to the Dawn targets the same way.

    # Keep SDL3 lean when aurora is enabled.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)

    # MinGW/WSL can choke on SDL precompiled headers; disable PCH for aurora/SDL.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    # Isolate aurora build products to a subdirectory under the main build dir.
    add_subdirectory(${MKW_AURORA_DIR} ${CMAKE_BINARY_DIR}/aurora-build EXCLUDE_FROM_ALL)
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

    # Keep upstream Aurora split: several sources intentionally use file-local
    # names that collide under this repo's aggressive unity build settings.
    set(AURORA_TARGETS aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi)
    foreach(t ${AURORA_TARGETS})
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES UNITY_BUILD OFF)
            target_compile_options(${t} PRIVATE -O3 -ffast-math -w -pipe)
            if(MKW_PLATFORM_WINDOWS)
                target_include_directories(${t} PRIVATE ${MKW_DX_STUB_DIR})
            endif()
            # Aurora's own sources include Windows headers and call std::min/max;
            # they relied on the old project-wide NOMINMAX that no longer leaks
            # into this subtree, so the define is applied per target here.
            target_compile_definitions(${t} PRIVATE NOMINMAX)
        endif()
    endforeach()

    # Frame interpolation filters guest NaN/Inf matrices out of a std::sort
    # comparator, which makes its std::isfinite guards load-bearing for memory
    # safety, not just match quality. -ffinite-math-only (part of -ffast-math
    # above) folds those guards to constants and poisons NaN float arguments
    # via nofpclass, so this one file opts back out of the finite-math
    # assumption; the file #errors if built without this. Source-scoped
    # COMPILE_OPTIONS append after the target options, so this overrides the
    # target's -ffast-math for exactly this TU.
    if(TARGET aurora_gx)
        set_source_files_properties("${MKW_AURORA_DIR}/lib/gx/frame_interpolation.cpp"
            TARGET_DIRECTORY aurora_gx
            PROPERTIES COMPILE_OPTIONS "-fno-finite-math-only")
    endif()

    if(MKW_NATIVE_PREBUILT_EXPORT_DIR)
        include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuiltExport.cmake")
    endif()
endif()

# Everything from here down is this project's own code, so this is where the
# project definitions enter the directory scope. A directory-scoped definition
# seeds the COMPILE_DEFINITIONS of targets as they are created, which means:
#   * aurora-main and its third-party tree were added above -> unaffected;
#   * mkw_runtime_common, the translated shard libraries and the products in
#     cmake/PublicProducts.cmake are all created below (that file is include()d,
#     not add_subdirectory()d, so it shares this directory scope) -> they pick
#     the definitions up exactly as before.
if(MKW_PROJECT_COMPILE_DEFINITIONS)
    add_compile_definitions(${MKW_PROJECT_COMPILE_DEFINITIONS})
endif()

# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build (measured
# at ~55 ms across the 100+ globs this build already re-verifies for SDL), and it
# buys correctness: without it, a newly added src/*.cpp that is not referenced by
# a registration file is silently never compiled and never errors. The stale-glob
# failure mode is worth far more than the milliseconds.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
if(MKW_TITLE_NATIVE_DIR)
    file(GLOB_RECURSE MKW_TITLE_NATIVE_SOURCES CONFIGURE_DEPENDS
        "${MKW_TITLE_NATIVE_DIR}/*.cpp")
    list(APPEND SOURCES ${MKW_TITLE_NATIVE_SOURCES})
    message(STATUS "Title-specific native HLE: ${MKW_TITLE_NATIVE_DIR}")
endif()

# A title may need to adjust host-backed state immediately before a statically
# translated SDK boundary, or service asynchronous hardware at translated loop
# backedges. Generated code reaches the shared ABI hook in both cases. Prefer the
# canonical WiiCompiled header name while retaining discovery of the historical
# MKW-named header and compile definition for existing projects.
set(WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH "")
if(MKW_TITLE_NATIVE_DIR)
    foreach(_wiicompiled_title_hook_name
            wiicompiled_title_runtime_call_options.h
            mkw_title_runtime_call_options.h)
        set(_wiicompiled_title_hook_candidate
            "${MKW_TITLE_NATIVE_DIR}/${_wiicompiled_title_hook_name}")
        if(EXISTS "${_wiicompiled_title_hook_candidate}")
            set(WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH
                "${_wiicompiled_title_hook_candidate}")
            break()
        endif()
    endforeach()
endif()

if(WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH)
    file(TO_CMAKE_PATH "${WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH}"
         WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH)
    # Preserve the old internal path variable for build glue that still inspects
    # it after configuration.
    set(MKW_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH
        "${WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH}")
    # Define both names. abi_bridge.h prefers WIICOMPILED_*; the MKW_* spelling
    # remains available to older source/configuration that still tests it.
    add_compile_definitions(
        WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER="${WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH}"
        MKW_TITLE_RUNTIME_CALL_OPTIONS_HEADER="${WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH}")
    message(STATUS
        "Title runtime-call options: ${WIICOMPILED_TITLE_RUNTIME_CALL_OPTIONS_HEADER_PATH}")
endif()
if(MKW_PLATFORM_MACOS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory.cpp")
    # HostContext's Apple Silicon backend is implemented in a small assembly
    # companion. It must be part of the product runtime as well as the
    # standalone context test; otherwise the final executable is missing
    # mkw_co_init/mkw_co_switch at link time.
    enable_language(ASM)
    list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
else()
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
endif()
set(MKW_PLATFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/platform/host_platform.cpp")
set(MKW_BASE_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/base_product.cpp")
set(MKW_RETRO_REWIND_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/retro_rewind_product.cpp")
# The host ISA guard is the one translation unit that must not receive the
# x86-64-v3 flags, so it gets its own object library instead of riding along in
# mkw_runtime_common. See cmake/PublicProducts.cmake and the file's own header.
set(MKW_CPU_BASELINE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/host_cpu_baseline.cpp")
list(REMOVE_ITEM SOURCES ${MKW_BASE_PRODUCT_SOURCE} ${MKW_RETRO_REWIND_PRODUCT_SOURCE}
     ${MKW_CPU_BASELINE_SOURCE} ${MKW_PLATFORM_SOURCE})

set(MKW_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}")
include(cmake/RuntimeTests.cmake)

# The translator emits the complete, content-addressed source graph. Consuming
# this one manifest keeps configure independent of the 28k generated function
# files and of optional Retro Rewind artifacts such as code.map.
if(MKW_BUILD_PRODUCTS)
    set(MKW_TRANSLATED_SHARD_MANIFEST
        "${MKW_GENERATED_DIR}/build_shards/shards.cmake"
        CACHE FILEPATH "Translator-owned aggregate shard manifest")
    # The prebuilt export only needs the aurora/third-party closure configured above, so a
    # packaging machine without a translation stops here instead of failing.
    if(MKW_NATIVE_PREBUILT_EXPORT_DIR AND NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(STATUS "No translator shard manifest; configuring the native prebuilt export only")
        return()
    endif()
    if(NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(FATAL_ERROR
            "Missing translator-owned shard manifest: ${MKW_TRANSLATED_SHARD_MANIFEST}. "
            "Run Translator.Cli emit-build-shards first; see translator/README.md.")
    endif()
    include("${MKW_TRANSLATED_SHARD_MANIFEST}")
    if(MKW_GENERIC_DOL_BOOT)
        add_compile_definitions(MKW_GENERIC_DOL_BOOT=1)
    endif()
    if(WIICOMPILED_GUEST_PLATFORM_RESOLVED STREQUAL "gamecube")
        if(NOT MKW_GENERIC_DOL_BOOT)
            message(FATAL_ERROR "GameCube guest products require MKW_GENERIC_DOL_BOOT")
        endif()
    endif()
    set(MKW_HAVE_RETRO_REWIND ${MKW_HAVE_RETRO_REWIND_SHARDS})
    message(STATUS
        "Translator graph: ${MKW_SHARED_BASE_FUNCTION_COUNT}/${MKW_BASE_FUNCTION_COUNT} base functions shared; "
        "${MKW_PROFILE_SENSITIVE_CALLER_COUNT} profile-sensitive callers; "
        "${MKW_RETRO_REWIND_FUNCTION_COUNT} Retro Rewind functions")

    set(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PublicProducts.cmake")
elseif(WIICOMPILED_RUNTIME_COMPILE_AUDIT)
    set(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    set(MKW_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/tests/fixtures/compile-audit")
    if(MKW_GENERIC_DOL_BOOT)
        add_compile_definitions(MKW_GENERIC_DOL_BOOT=1)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/RuntimeObjects.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/RuntimeArchitecture.cmake")
    message(STATUS "Game-free runtime compile audit enabled (no runnable guest product)")
else()
    if(MKW_PLATFORM_MACOS)
        # Compile-only audit of native runtime sources. It deliberately avoids
        # translated products until their host dependencies are portable.
        # These sources depend on generated/RuntimeConfig.h, which is emitted for
        # a particular game by the translator and is intentionally unavailable
        # in this platform-only configuration.
        set(MKW_MACOS_NATIVE_AUDIT_SOURCES ${SOURCES})
        list(REMOVE_ITEM MKW_MACOS_NATIVE_AUDIT_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/src/abi_bridge.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/hle/os/os_alarm.cpp")
        add_library(mkw_macos_native_compile OBJECT ${MKW_MACOS_NATIVE_AUDIT_SOURCES})
        target_include_directories(mkw_macos_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/include")
        target_compile_features(mkw_macos_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(mkw_macos_native_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
        target_link_libraries(mkw_macos_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            mkw::pugixml mkw::toml11 mkw::cryptopp)
        set_target_properties(mkw_macos_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    add_custom_target(mkw_platform_paths_check DEPENDS mkw_platform)
    message(STATUS "Translated product targets disabled (MKW_BUILD_PRODUCTS=OFF)")
endif()

if(WIICOMPILED_GRAPHICS_TESTS)
    add_subdirectory("${MKW_AURORA_DIR}/tests" "${CMAKE_BINARY_DIR}/aurora-tests" EXCLUDE_FROM_ALL)
endif()
