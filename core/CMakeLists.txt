# Core abstractions library: jobs, events, filesystem, process, hardware, settings,
# logging, errors. No engine (FFmpeg/Python) or UI code lives here -- see engines/ and
# app/. One target for all of core/ is a deliberate Phase 1 simplification (spec section
# 41: don't overengineer); split into finer-grained targets only if a real need appears.
file(GLOB_RECURSE MEDIATOOL_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(GLOB_RECURSE MEDIATOOL_CORE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(mtcore STATIC ${MEDIATOOL_CORE_SOURCES} ${MEDIATOOL_CORE_HEADERS})

target_include_directories(mtcore PUBLIC
    "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(mtcore PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    reproc++
)

if(WIN32)
    target_link_libraries(mtcore PUBLIC dxgi d3d11)
endif()
