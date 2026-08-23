# Engine implementations that sit behind core/media, core/downloads interfaces:
# FFmpeg process engine, image engine (stub), document engine (stub), yt-dlp provider.
file(GLOB_RECURSE MEDIATOOL_ENGINES_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(GLOB_RECURSE MEDIATOOL_ENGINES_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(mtengines STATIC ${MEDIATOOL_ENGINES_SOURCES} ${MEDIATOOL_ENGINES_HEADERS})

target_include_directories(mtengines PUBLIC
    "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(mtengines PUBLIC mtcore)
