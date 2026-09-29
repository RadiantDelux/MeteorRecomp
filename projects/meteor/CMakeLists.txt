cmake_minimum_required(VERSION 3.25)
project(meteor LANGUAGES C CXX)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Generated controller prompt packs stay outside the repository.
set(METEOR_CONTROLLER_PROMPTS_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../local-data/meteor/controller_prompts")
set(WIICOMPILED_CONTROLLER_PROMPTS_DIR "${METEOR_CONTROLLER_PROMPTS_DIR}")
set(WIICOMPILED_WINDOW_ICON
    "${CMAKE_CURRENT_LIST_DIR}/../../icon.png")
set(WIICOMPILED_WINDOWS_ICON_ICO
    "${CMAKE_CURRENT_LIST_DIR}/assets/meteor_icon.ico")
set(MKW_TRANSLATED_OPT_LEVEL -O3)
set(MKW_HOST_TUNE_FLAG -mtune=native)
# Battle staging needs 8 MiB of storage to avoid batch rotations.
add_compile_definitions(
    AURORA_LOW_MEMORY_PIPELINE_PREWARM=1
    AURORA_UNIFORM_BUFFER_SIZE=8388608ull
    AURORA_VERTEX_BUFFER_SIZE=2097152ull
    AURORA_INDEX_BUFFER_SIZE=1048576ull
    AURORA_STORAGE_BUFFER_SIZE=8388608ull)
if(NOT EXISTS "${METEOR_CONTROLLER_PROMPTS_DIR}")
    message(WARNING
        "Meteor controller prompt packs are not generated yet: ${METEOR_CONTROLLER_PROMPTS_DIR}")
endif()

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../runtime" "${CMAKE_BINARY_DIR}/runtime")
