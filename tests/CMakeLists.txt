# Single GoogleTest binary covering tests/core, tests/ipc, tests/media, tests/downloads
# (spec section 36). tests/python is a separate, dependency-free `unittest` suite -- see
# python/downloader/requirements.txt and docs/development.md.
find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE MEDIATOOL_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ipc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/media/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/downloads/*.cpp"
)

add_executable(mediatool_tests ${MEDIATOOL_TEST_SOURCES})

target_link_libraries(mediatool_tests PRIVATE
    mtcore
    mtengines
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mediatool_tests)
