cmake_minimum_required(VERSION 3.21)

project(Gravity
    VERSION 0.1.0
    DESCRIPTION "Gravity: local-first media downloader, converter, and compressor - C++ core"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# MinGW builds are Phase 1's supported toolchain (no MSVC/Visual Studio dependency --
# see docs/development.md). Link the runtime statically so mediatool-core.exe runs
# standalone without shipping libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll
# next to it.
if(MINGW)
    add_compile_options(-Wall -Wextra)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

option(MEDIATOOL_BUILD_TESTS "Build the GoogleTest suite" ON)

# AddressSanitizer + UndefinedBehaviorSanitizer, for finding the class of bug the stress
# suite provokes but cannot always observe -- a data race or a use-after-free that happens
# to not crash on this run.
#
# Deliberately opt-in and deliberately not part of any default preset: on the MinGW-w64
# toolchain this project ships with, GCC's ASan runtime is not reliably available and TSan
# does not exist for Windows at all. The supported way to run this is a Linux or WSL build,
# which is enough for the concurrency code -- none of it is Windows-specific.
#
#   cmake --preset sanitizers-linux && cmake --build --preset sanitizers-linux \
#     && ctest --preset sanitizers-linux
#
# The `sanitizers-debug` preset is the Windows/MinGW spelling and exists only for a machine
# where that toolchain's ASan does work; `sanitizers-linux` is the one that runs on a stock
# Linux/WSL box, which is why it does not inherit the MinGW compilers or the vcpkg
# mingw-static triplet. scripts/ci-local.ps1 does not have a sanitizer stage -- this is a
# deliberate, manually-invoked build, not part of per-commit validation.
option(MEDIATOOL_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(MEDIATOOL_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "MEDIATOOL_ENABLE_SANITIZERS is not supported on MSVC here; use a Linux/WSL build.")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(reproc++ CONFIG REQUIRED)

add_subdirectory(core)
add_subdirectory(engines)
add_subdirectory(app/core)

if(MEDIATOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
