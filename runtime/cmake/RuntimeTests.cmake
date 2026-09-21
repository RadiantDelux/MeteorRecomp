# This deliberately small library contains host services that are safe to
# validate before guest memory and fiber work makes a full runtime build viable.
add_library(mkw_platform STATIC "${MKW_PLATFORM_SOURCE}")
target_include_directories(mkw_platform PUBLIC "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_platform PUBLIC cxx_std_17)
set_target_properties(mkw_platform PROPERTIES UNITY_BUILD OFF)

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
enable_testing()
if(WIN32)
    target_link_libraries(mkw_platform PUBLIC shell32 ole32 uuid)
endif()
add_executable(mkw_platform_paths_tests "${MKW_RUNTIME_DIR}/tests/platform_paths_tests.cpp")
target_link_libraries(mkw_platform_paths_tests PRIVATE mkw_platform)
target_compile_features(mkw_platform_paths_tests PRIVATE cxx_std_17)
add_test(NAME mkw_platform_paths_tests COMMAND mkw_platform_paths_tests)

add_executable(mkw_dynamic_module_tests
    "${MKW_RUNTIME_DIR}/tests/dynamic_module_tests.cpp"
    "${MKW_RUNTIME_DIR}/src/dynamic_module.cpp")
target_include_directories(mkw_dynamic_module_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_dynamic_module_tests PRIVATE cxx_std_17)
# These tests use assert for both setup and verification. Release must not
# compile those expressions away and report an empty test as a success.
if(MSVC)
    target_compile_options(mkw_dynamic_module_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(mkw_dynamic_module_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME mkw_dynamic_module_tests COMMAND mkw_dynamic_module_tests)

add_executable(mkw_controller_status_contract_tests
    "${MKW_RUNTIME_DIR}/tests/controller_status_contract_tests.cpp")
target_include_directories(mkw_controller_status_contract_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_controller_status_contract_tests PRIVATE cxx_std_17)
add_test(NAME mkw_controller_status_contract_tests COMMAND mkw_controller_status_contract_tests)

add_executable(mkw_locked_cache_dma_tests
    "${MKW_RUNTIME_DIR}/tests/locked_cache_dma_tests.cpp")
target_include_directories(mkw_locked_cache_dma_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_locked_cache_dma_tests PRIVATE cxx_std_17)
add_test(NAME mkw_locked_cache_dma_tests COMMAND mkw_locked_cache_dma_tests)

add_executable(mkw_data_cache_writeback_tests
    "${MKW_RUNTIME_DIR}/tests/data_cache_writeback_tests.cpp")
target_include_directories(mkw_data_cache_writeback_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_data_cache_writeback_tests PRIVATE cxx_std_17)
add_test(NAME mkw_data_cache_writeback_tests COMMAND mkw_data_cache_writeback_tests)

add_executable(mkw_float_load_tests
    "${MKW_RUNTIME_DIR}/tests/float_load_tests.cpp")
target_include_directories(mkw_float_load_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_float_load_tests PRIVATE cxx_std_20)
target_compile_options(mkw_float_load_tests PRIVATE ${MKW_TRANSLATED_PPC_FP_OPTIONS})
add_test(NAME mkw_float_load_tests COMMAND mkw_float_load_tests)

add_executable(mkw_gamecube_memory_card_tests
    "${MKW_RUNTIME_DIR}/tests/gamecube_memory_card_tests.cpp")
target_include_directories(mkw_gamecube_memory_card_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
target_compile_features(mkw_gamecube_memory_card_tests PRIVATE cxx_std_20)
add_test(NAME mkw_gamecube_memory_card_tests COMMAND mkw_gamecube_memory_card_tests)

add_executable(mkw_gamecube_audio_tests "${MKW_RUNTIME_DIR}/tests/gamecube_audio_tests.cpp")
target_include_directories(mkw_gamecube_audio_tests PRIVATE
    "${MKW_RUNTIME_DIR}/include" "${MKW_RUNTIME_DIR}/src/hle/audio")
target_compile_features(mkw_gamecube_audio_tests PRIVATE cxx_std_20)
target_compile_definitions(mkw_gamecube_audio_tests PRIVATE MKW_GAMECUBE_DOL_BOOT=1)
add_test(NAME mkw_gamecube_audio_tests COMMAND mkw_gamecube_audio_tests)

# HostContext deliberately keeps the platform-specific context primitive out
# of fiber_manager.cpp. Exercise the Linux libco handoff directly so future
# refactors cannot silently remove its headers, implementation, or link edge.
if(MKW_PLATFORM_LINUX)
    add_executable(mkw_linux_host_context_tests
        "${MKW_RUNTIME_DIR}/tests/host_context_tests.cpp"
        "${MKW_RUNTIME_DIR}/src/host_context.cpp")
    target_include_directories(mkw_linux_host_context_tests PRIVATE
        "${MKW_RUNTIME_DIR}/include"
        "${MKW_RUNTIME_DIR}/third_party/libco")
    target_compile_features(mkw_linux_host_context_tests PRIVATE cxx_std_17)
    target_link_libraries(mkw_linux_host_context_tests PRIVATE mkw::libco)
    add_test(NAME mkw_linux_host_context_tests COMMAND mkw_linux_host_context_tests)
endif()

if(MKW_PLATFORM_MACOS)
    # Exercise the Apple Silicon context ABI and the public host-memory
    # contracts separately from translated products.
    enable_language(ASM)
    add_executable(mkw_macos_context_abi_tests
        "${MKW_RUNTIME_DIR}/tests/macos_context_abi_tests.cpp"
        "${MKW_RUNTIME_DIR}/src/platform/macos/co_switch.S")
    target_compile_features(mkw_macos_context_abi_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_context_abi_tests COMMAND mkw_macos_context_abi_tests)

    add_executable(mkw_macos_host_context_tests
        "${MKW_RUNTIME_DIR}/tests/host_context_tests.cpp"
        "${MKW_RUNTIME_DIR}/src/host_context.cpp"
        "${MKW_RUNTIME_DIR}/src/platform/macos/co_switch.S")
    target_include_directories(mkw_macos_host_context_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
    target_compile_features(mkw_macos_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_host_context_tests COMMAND mkw_macos_host_context_tests)

    add_executable(mkw_macos_guest_flat_memory_tests
        "${MKW_RUNTIME_DIR}/tests/macos_guest_flat_memory_tests.cpp"
        "${MKW_RUNTIME_DIR}/src/guest_flat_memory_macos.cpp")
    target_include_directories(mkw_macos_guest_flat_memory_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
    target_compile_features(mkw_macos_guest_flat_memory_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_guest_flat_memory_tests COMMAND mkw_macos_guest_flat_memory_tests)
endif()

# Cross-project correctness contracts; no generated game inputs are needed.
foreach(test IN ITEMS psq_scale audio_dma_timing audio_playback_continuity interpolation_worker_pool interpolation_cadence presentation_schedule meteor_loop_service_cadence guest_clock host_presentation_pacer card_checkpoint disc_alignment project_paths)
    if(test STREQUAL "psq_scale")
        set(source "psq_scale_tests.cpp")
    else()
        set(source "${test}_test.cpp")
    endif()
    add_executable(mkw_${test}_tests "${MKW_RUNTIME_DIR}/tests/${source}")
    target_include_directories(mkw_${test}_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
    target_compile_features(mkw_${test}_tests PRIVATE cxx_std_20)
    add_test(NAME mkw_${test}_tests COMMAND mkw_${test}_tests)
endforeach()
target_link_libraries(mkw_project_paths_tests PRIVATE mkw_platform)
target_compile_definitions(mkw_guest_clock_tests PRIVATE WIICOMPILED_GUEST_PLATFORM=2 MKW_GAMECUBE_DOL_BOOT=1)
target_compile_options(mkw_psq_scale_tests PRIVATE ${MKW_TRANSLATED_PPC_FP_OPTIONS})

add_executable(mkw_indirect_dispatch_tests
    "${MKW_RUNTIME_DIR}/tests/indirect_dispatch_test.cpp"
    "${MKW_RUNTIME_DIR}/src/abi_bridge.cpp")
target_include_directories(mkw_indirect_dispatch_tests PRIVATE
    "${MKW_RUNTIME_DIR}/include" "${MKW_RUNTIME_DIR}/tests/fixtures/compile-audit")
target_compile_features(mkw_indirect_dispatch_tests PRIVATE cxx_std_20)
target_compile_definitions(mkw_indirect_dispatch_tests PRIVATE MKW_GENERIC_DOL_BOOT=1)
add_test(NAME mkw_indirect_dispatch_tests COMMAND mkw_indirect_dispatch_tests)

if(WIN32)
    add_executable(mkw_windows_host_context_tests
        "${MKW_RUNTIME_DIR}/tests/host_context_tests.cpp"
        "${MKW_RUNTIME_DIR}/src/host_context.cpp")
    target_include_directories(mkw_windows_host_context_tests PRIVATE "${MKW_RUNTIME_DIR}/include")
    target_compile_features(mkw_windows_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_windows_host_context_tests COMMAND mkw_windows_host_context_tests)
endif()

# Many contract tests use assert for setup as well as verification.
get_property(_mkw_contract_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS _mkw_contract_tests)
    if(TARGET ${test})
        if(MSVC)
            target_compile_options(${test} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test} PRIVATE -UNDEBUG)
        endif()
    endif()
endforeach()
if(WIN32)
    get_filename_component(_mkw_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(${_mkw_contract_tests} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_mkw_compiler_bin}")
endif()
set_tests_properties(${_mkw_contract_tests} PROPERTIES TIMEOUT 120)
