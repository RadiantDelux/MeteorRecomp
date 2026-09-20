# Windows and Linux x86_64 share the x86-64-v3 floor that the CPU baseline
# object above checks. AArch64 builds are compiled locally for the host that
# will run them, so both Linux and Apple Silicon use the compiler's native CPU
# tuning rather than leaving target-specific performance on the table.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(MKW_BASELINE_ARCH_FLAG -march=x86-64-v3)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(MKW_BASELINE_ARCH_FLAG -mcpu=native)
else()
    set(MKW_BASELINE_ARCH_FLAG "")
endif()

set(MKW_ALL_BUILD_TARGETS
    mkw_runtime_common mkw_base_shared mkw_dynamic_modules mkw_base_sensitive mkw_retro_sensitive
    mkw_retro_rewind_functions WiiCompiled RetroRewind)
foreach(target IN LISTS MKW_ALL_BUILD_TARGETS)
    if(TARGET ${target} AND MKW_BASELINE_ARCH_FLAG)
        target_compile_options(${target} PRIVATE ${MKW_BASELINE_ARCH_FLAG})
    endif()
endforeach()
