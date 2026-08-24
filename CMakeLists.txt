cmake_minimum_required(VERSION 3.21)

# VERSION here, app/desktop/src-tauri/Cargo.toml's package.version, and
# app/frontend/package.json's version are three independent ecosystems' manifests and
# can't literally share one file -- they are kept numerically equal by convention and
# checked by scripts/check-versions.* (see docs/phase-7.md "Versioning").
project(Gravity
    VERSION 0.1.0
    DESCRIPTION "Gravity -- a local-first media downloader, converter, and compressor (C++ core)"
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
