# ============================================================================
# audiostack - audio capture + IPC C++ library & daemon
#
# Targets:
#   - Rubik Pi 3 (QCS6490) / Qualcomm Linux (Debian 13 based, arm64)
#   - Fedora 44 (x86_64)  — same sources build on both
#
# Dependencies: g++ (C++17), cmake >= 3.16, pkg-config, alsa-lib, pthread, rt
#   Debian/Qualcomm Linux: sudo apt install build-essential cmake libasound2-dev
#   Fedora:                sudo dnf install gcc-c++ cmake make alsa-lib-devel
#
# Build: ./scripts/build.sh   or
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
pkg_check_modules(ALSA REQUIRED alsa)

# ---------------------------------------------------------------- library ---
add_library(astk STATIC
  src/config.cpp
  src/alsa_capture.cpp
  src/http_client.cpp
  src/shm_ring.cpp
  src/ipc_server.cpp
)
target_include_directories(astk PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${ALSA_INCLUDE_DIRS}
)
target_link_libraries(astk PUBLIC ${ALSA_LIBRARIES} pthread rt)
target_compile_options(astk PUBLIC ${ALSA_CFLAGS_OTHER})

# ----------------------------------------------------------------- daemon ---
add_executable(audiostackd src/daemon.cpp)
target_link_libraries(audiostackd PRIVATE astk)

# ------------------------------------------------------------------ tools ---
option(ASTK_BUILD_TOOLS "Build helper tools" ON)
if(ASTK_BUILD_TOOLS)
  foreach(tool list_devices capture_wav ctl)
    add_executable(astk-${tool} tools/${tool}.cpp)
    target_link_libraries(astk-${tool} PRIVATE astk)
  endforeach()
endif()

# ---------------------------------------------------------------- install ---
install(TARGETS audiostackd DESTINATION bin)
if(ASTK_BUILD_TOOLS)
  install(TARGETS astk-list-devices astk-capture-wav astk-ctl DESTINATION bin)
endif()
install(DIRECTORY include/audiostack DESTINATION include)
install(FILES config/audiostack.conf DESTINATION etc/audiostack)
