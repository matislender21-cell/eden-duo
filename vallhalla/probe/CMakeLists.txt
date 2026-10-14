# VAllhalla probe module (VA-11 Hall-A, 010086500D3C8000): a standalone shared object
# loaded by Eden Duo's dual-screen runtime. Build for Android arm64 with the NDK toolchain
# (-DANDROID_STL=c++_static); on a desktop host it also builds the fake-memory test.
cmake_minimum_required(VERSION 3.22)
project(vallhalla_probe CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EDEN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src" CACHE PATH "Eden Duo src directory")

add_library(probe SHARED probe.cpp)
target_include_directories(probe PRIVATE "${EDEN_SRC}" "${EDEN_SRC}/../externals/stb")
set_target_properties(probe PROPERTIES PREFIX "" OUTPUT_NAME "010086500D3C8000"
                      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.exports"
     "{ global: eden_dsmod_get_module; eden_dsmod_get_extensions; local: *; };\n")
target_link_options(probe PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/probe.exports" "-Wl,--no-undefined")
if(ANDROID)
    if(NOT ANDROID_STL STREQUAL "c++_static")
        message(FATAL_ERROR "Build with -DANDROID_STL=c++_static")
    endif()
    target_link_options(probe PRIVATE "-Wl,--exclude-libs,ALL")
else()
    add_executable(probe_test test_probe.cpp probe.cpp)
    target_include_directories(probe_test PRIVATE "${EDEN_SRC}" "${EDEN_SRC}/../externals/stb")
endif()
