# ============================================================================
# audiostack - audio capture + IPC C++ library & daemon
#
# Targets:
#   - RUBIK Pi 3 (QCS6490) / Ubuntu for Qualcomm (arm64)
#   - Fedora 44 (x86_64)  — same sources build on both
#
# Dependencies: g++ (C++17), cmake >= 3.16, pkg-config, alsa-lib, pthread, rt
#   Ubuntu for Qualcomm: sudo apt install build-essential cmake libasound2-dev
#   Fedora:                sudo dnf install gcc-c++ cmake make alsa-lib-devel
#
# Build: bash scripts/build.sh   or
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(audiostack VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
option(ASTK_ENABLE_RNNOISE "Build RNNoise noise suppression with SpeexDSP resampling" OFF)

# ---------------------------------------------------------------- library ---
add_library(astk STATIC
  src/config.cpp
  src/alsa_capture.cpp
  src/http_client.cpp
  src/shm_ring.cpp
  src/ipc_server.cpp
  src/preprocess.cpp
)
target_include_directories(astk PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(astk PUBLIC PkgConfig::ALSA pthread rt)
if(ASTK_ENABLE_RNNOISE)
  pkg_check_modules(RNNOISE REQUIRED IMPORTED_TARGET rnnoise>=0.2)
  pkg_check_modules(SPEEXDSP REQUIRED IMPORTED_TARGET speexdsp)
  target_compile_definitions(astk PRIVATE ASTK_ENABLE_RNNOISE=1)
  target_link_libraries(astk PRIVATE PkgConfig::RNNOISE PkgConfig::SPEEXDSP m)
endif()

# ----------------------------------------------------------------- daemon ---
add_executable(audiostackd src/daemon.cpp)
target_link_libraries(audiostackd PRIVATE astk)

# ------------------------------------------------------------------ tools ---
option(ASTK_BUILD_TOOLS "Build helper tools" ON)
if(ASTK_BUILD_TOOLS)
  foreach(tool list_devices capture_wav ctl)
    string(REPLACE "_" "-" tool_name ${tool})
    add_executable(astk-${tool_name} tools/${tool}.cpp)
    target_link_libraries(astk-${tool_name} PRIVATE astk)
  endforeach()
endif()

# ------------------------------------------------------------------ tests ---
include(CTest)
if(BUILD_TESTING)
  add_executable(astk-preprocess-test tests/test_preprocess.cpp)
  target_link_libraries(astk-preprocess-test PRIVATE astk)
  add_test(NAME preprocess COMMAND astk-preprocess-test)
  add_executable(astk-protocol-test tests/test_protocol.cpp)
  target_link_libraries(astk-protocol-test PRIVATE astk)
  add_test(NAME protocol COMMAND astk-protocol-test)
endif()

# ---------------------------------------------------------------- install ---
install(TARGETS audiostackd DESTINATION bin)
if(ASTK_BUILD_TOOLS)
  install(TARGETS astk-list-devices astk-capture-wav astk-ctl DESTINATION bin)
endif()
install(DIRECTORY include/audiostack DESTINATION include)
install(FILES config/audiostack.conf config/xvf3800-16k2ch.conf
        config/xvf3800-16k6ch.conf config/xvf3800-48k2ch.conf
        DESTINATION etc/audiostack)
