cmake_minimum_required(VERSION 3.25)
project(meteor LANGUAGES C CXX)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../runtime" "${CMAKE_BINARY_DIR}/runtime")
