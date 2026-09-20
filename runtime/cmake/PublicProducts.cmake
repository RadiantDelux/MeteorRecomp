# Public WiiCompiled product graph.
#
# The translator owns the translated build graph. Mario Kart's profile-neutral
# functions are compiled once into mkw_base_shared; only callers whose direct
# ABI differs between profiles receive small base/RR variants.

include("${CMAKE_CURRENT_LIST_DIR}/RuntimeObjects.cmake")

if(NOT MKW_BASE_COMMON_SHARDS)
    message(FATAL_ERROR "Translator build graph contains no shared base shards")
endif()

add_library(mkw_base_shared STATIC ${MKW_BASE_COMMON_SHARDS})
mkw_configure_translated_target(mkw_base_shared)
target_precompile_headers(mkw_base_shared PRIVATE "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")

# Runtime-loaded Wii code (RSO/overlay templates) is pretranslated into its own
# namespace. These sources deliberately do not enter the absolute-address base
# registration/dispatch shards; their registrar TUs publish offset-keyed
# DynamicModule::TemplateRecord records instead.
file(GLOB_RECURSE MKW_DYNAMIC_MODULE_SOURCES CONFIGURE_DEPENDS
    "${MKW_GENERATED_DIR}/dynamic_modules/*/cpp/*.cpp")
if(MKW_DYNAMIC_MODULE_SOURCES)
    add_library(mkw_dynamic_modules OBJECT ${MKW_DYNAMIC_MODULE_SOURCES})
    mkw_configure_translated_target(mkw_dynamic_modules)
    target_precompile_headers(mkw_dynamic_modules REUSE_FROM mkw_base_shared)
endif()

if(MKW_BASE_PORTABLE_SENSITIVE_SHARDS)
    add_library(mkw_base_sensitive OBJECT ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS})
    mkw_configure_translated_target(mkw_base_sensitive)
    target_precompile_headers(mkw_base_sensitive REUSE_FROM mkw_base_shared)
endif()

if(MKW_HAVE_RETRO_REWIND)
    if(MKW_RETRO_PORTABLE_SENSITIVE_SHARDS)
        add_library(mkw_retro_sensitive OBJECT ${MKW_RETRO_PORTABLE_SENSITIVE_SHARDS})
        mkw_configure_translated_target(mkw_retro_sensitive)
        target_precompile_headers(mkw_retro_sensitive REUSE_FROM mkw_base_shared)
    endif()

    set(MKW_RETRO_TRANSLATED_SOURCES ${MKW_RETRO_MOD_SHARDS} ${MKW_RETRO_EXTRA_SOURCES})
    set(MKW_RETRO_BLOB_OBJECTS)
    foreach(source IN LISTS MKW_RETRO_EXTRA_SOURCES)
        if(source MATCHES "\\.S$")
            enable_language(ASM)
            set_source_files_properties("${source}" PROPERTIES LANGUAGE ASM SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endforeach()
    add_library(mkw_retro_rewind_functions OBJECT ${MKW_RETRO_TRANSLATED_SOURCES})
    mkw_configure_translated_target(mkw_retro_rewind_functions)
    target_precompile_headers(mkw_retro_rewind_functions REUSE_FROM mkw_base_shared)
endif()

function(mkw_configure_product target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_runtime_common>)
    if(TARGET mkw_dynamic_modules)
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_dynamic_modules>)
    endif()
    # Startup CPU check. Must stay a separate object library so it keeps the
    # plain baseline ISA while everything around it is built for x86-64-v3.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_cpu_baseline>)
    endif()
    target_include_directories(${target} PRIVATE
        "${MKW_RUNTIME_SOURCE_DIR}/include"
        "${MKW_RUNTIME_SOURCE_DIR}/src"
        "${MKW_GENERATED_DIR}"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${MKW_RUNTIME_SOURCE_DIR}/.."
        "${MKW_RUNTIME_SOURCE_DIR}/../aurora-main/include")
    target_compile_definitions(${target} PRIVATE
        SDL_MAIN_HANDLED _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION TARGET_PC)
    target_compile_features(${target} PRIVATE cxx_std_20)
    mkw_apply_common_compile_options(${target})
    # The dispatch-table and registration shards compile inside the product target itself and
    # include the same fat translated headers; bound them by the same pool.
    mkw_bound_translated_compiles(${target})
    target_link_libraries(${target} PRIVATE
        mkw_platform mkw_base_shared mkw::pugixml mkw::toml11 mkw::cryptopp)

    target_link_libraries(${target} PRIVATE
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)
    if(MKW_PLATFORM_MACOS)
        target_link_libraries(${target} PRIVATE
            "${MKW_IOKIT_FRAMEWORK}" "${MKW_COREFOUNDATION_FRAMEWORK}")
    endif()
    if(EXISTS "${MKW_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        include("${MKW_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        aurora_copy_runtime_dlls(${target})
    endif()
    if(TARGET sqlite3)
        get_target_property(MKW_SQLITE_TARGET_TYPE sqlite3 TYPE)
    endif()
    if(TARGET sqlite3 AND
       (MKW_SQLITE_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
        MKW_SQLITE_TARGET_TYPE STREQUAL "MODULE_LIBRARY"))
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sqlite3> $<TARGET_FILE_DIR:${target}>)
    endif()

    if(MKW_PLATFORM_WINDOWS)
        target_link_libraries(${target} PRIVATE
            dbghelp user32 winmm ws2_32 iphlpapi secur32 crypt32 windowsapp)

        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(MKW_PLATFORM_LINUX)
        # mkw_runtime_common is an OBJECT library: WiiCompiled/RetroRewind only pull in its .o
        # files via $<TARGET_OBJECTS:>, which does not propagate mkw_runtime_common's own
        # target_link_libraries (object libraries don't carry usage requirements to a consumer
        # that isn't itself linked against as a target). fiber_manager.cpp's co_* calls live in
        # those objects, so the actual executable link needs mkw::libco directly, same as it
        # needs it independently of that first `if(WIN32)` branch above. ${CMAKE_DL_LIBS} is
        # here for the same reason: music_attenuation.cpp's dlopen(libdbus-1) lives in those
        # objects (empty string on glibc >= 2.34, where dl* is in libc).
        target_link_libraries(${target} PRIVATE mkw::libco ${CMAKE_DL_LIBS})
    endif()
    if(MKW_PLATFORM_WINDOWS)
        foreach(runtime_dll libc++.dll libunwind.dll)
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" "--print-file-name=${runtime_dll}"
                OUTPUT_VARIABLE runtime_dll_path
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT EXISTS "${runtime_dll_path}")
                get_filename_component(mkw_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
                set(runtime_dll_path "${mkw_compiler_bin}/${runtime_dll}")
            endif()
            if(NOT EXISTS "${runtime_dll_path}")
                message(FATAL_ERROR "llvm-mingw runtime DLL not found: ${runtime_dll}")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_dll_path}" $<TARGET_FILE_DIR:${target}>)
        endforeach()
    endif()

    # Managed NAND bootstrap is a Wii capability, not a generic product asset.
    # GameCube projects must not depend on or package Wii filesystem state.
    if(WIICOMPILED_GUEST_PLATFORM_RESOLVED STREQUAL "wii")
        set(MKW_WII_BOOTSTRAP_SOURCE_DIR "${MKW_RUNTIME_SOURCE_DIR}/assets/wii")
        if(NOT EXISTS "${MKW_WII_BOOTSTRAP_SOURCE_DIR}/shared2/wc24")
            message(FATAL_ERROR "Missing Wii first-run bootstrap payload: ${MKW_WII_BOOTSTRAP_SOURCE_DIR}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MKW_WII_BOOTSTRAP_SOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>/wii_bootstrap")
    endif()

    set(MKW_DSP_COEFFICIENT_ROM "${MKW_RUNTIME_SOURCE_DIR}/assets/dsp/dsp_coef.bin")
    if(NOT EXISTS "${MKW_DSP_COEFFICIENT_ROM}")
        message(FATAL_ERROR "Missing DSP coefficient ROM: ${MKW_DSP_COEFFICIENT_ROM}")
    endif()
    file(SHA256 "${MKW_DSP_COEFFICIENT_ROM}" MKW_DSP_COEFFICIENT_ROM_SHA256)
    if(NOT MKW_DSP_COEFFICIENT_ROM_SHA256 STREQUAL
       "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179")
        message(FATAL_ERROR "DSP coefficient ROM hash mismatch: ${MKW_DSP_COEFFICIENT_ROM_SHA256}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MKW_DSP_COEFFICIENT_ROM}" "$<TARGET_FILE_DIR:${target}>/dsp_coef.bin")

    set(MKW_IPL_FONT_DIR "${MKW_RUNTIME_SOURCE_DIR}/assets/ipl")
    foreach(ipl_font font_japanese.bin font_western.bin)
        if(NOT EXISTS "${MKW_IPL_FONT_DIR}/${ipl_font}")
            message(FATAL_ERROR "Missing free IPL font asset: ${MKW_IPL_FONT_DIR}/${ipl_font}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MKW_IPL_FONT_DIR}/${ipl_font}" "$<TARGET_FILE_DIR:${target}>/${ipl_font}")
    endforeach()

    # Aurora imports this portable recipe database into each user's writable
    # pipeline cache. Keep the upstream filename so its default resourcesPath
    # lookup works without application-specific configuration.
    set(MKW_INITIAL_PIPELINE_CACHE
        "${MKW_RUNTIME_SOURCE_DIR}/assets/pipeline/initial_pipeline_cache.db")
    if(NOT EXISTS "${MKW_INITIAL_PIPELINE_CACHE}")
        message(FATAL_ERROR "Missing transferable Aurora pipeline cache: ${MKW_INITIAL_PIPELINE_CACHE}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MKW_INITIAL_PIPELINE_CACHE}"
        "$<TARGET_FILE_DIR:${target}>/initial_pipeline_cache.db")
endfunction()

add_executable(WiiCompiled "${MKW_BASE_PRODUCT_SOURCE}" ${MKW_BASE_REGISTRATION_SOURCES})
mkw_configure_product(WiiCompiled)
set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME "${WIICOMPILED_EXECUTABLE_BASENAME}")
target_precompile_headers(WiiCompiled PRIVATE
    "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
if(TARGET mkw_base_sensitive)
    target_sources(WiiCompiled PRIVATE $<TARGET_OBJECTS:mkw_base_sensitive>)
endif()

if(MKW_HAVE_RETRO_REWIND)
    add_executable(RetroRewind "${MKW_RETRO_REWIND_PRODUCT_SOURCE}" ${MKW_RETRO_REGISTRATION_SOURCES})
    mkw_configure_product(RetroRewind)
    target_precompile_headers(RetroRewind REUSE_FROM WiiCompiled)
    if(TARGET mkw_retro_sensitive)
        target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_sensitive>)
    endif()
    target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_rewind_functions>)
    if(MKW_RETRO_BLOB_OBJECTS)
        target_sources(RetroRewind PRIVATE ${MKW_RETRO_BLOB_OBJECTS})
    endif()
    add_custom_target(mkw_release DEPENDS WiiCompiled RetroRewind)
else()
    add_custom_target(mkw_release DEPENDS WiiCompiled)
    message(STATUS "RetroRewind target disabled (run translate-mod and emit-build-shards)")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/RuntimeArchitecture.cmake")
