# Single GoogleTest binary covering tests/core, tests/ipc, tests/media, tests/downloads,
# tests/security
# (spec section 36). tests/python is a separate, dependency-free `unittest` suite -- see
# python/downloader/requirements.txt and docs/development.md.
find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE MEDIATOOL_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ipc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/media/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/downloads/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/security/*.cpp"
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

# --- Integration and stress suites ------------------------------------------------------
# Separate binaries, not extra files in mediatool_tests, because they are slow and are run
# deliberately (scripts/ci-local.ps1 -IntegrationTest / -StressTest) rather than on every
# build. Keeping them out of the default suite is what keeps that suite fast enough to run
# constantly, which is the only reason it gets run at all.

find_package(Python3 COMPONENTS Interpreter QUIET)
set(MEDIATOOL_TEST_PYTHON "${Python3_EXECUTABLE}")
if(NOT MEDIATOOL_TEST_PYTHON)
    set(MEDIATOOL_TEST_PYTHON "${CMAKE_COMMAND}")  # any real, existing executable will do
endif()

add_executable(mediatool_integration_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/CoreProcessFixture.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/IpcProtocolTest.cpp"
)
target_include_directories(mediatool_integration_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(mediatool_integration_tests PRIVATE
    mtcore
    GTest::gtest
    GTest::gtest_main
)
# The child process these tests drive, and two real existing files for the
# downloader-availability gate to find (nothing here ever runs them).
target_compile_definitions(mediatool_integration_tests PRIVATE
    MEDIATOOL_CORE_EXECUTABLE="$<TARGET_FILE:mediatool-core>"
    MEDIATOOL_TEST_PYTHON="${MEDIATOOL_TEST_PYTHON}"
    MEDIATOOL_TEST_DOWNLOADER="${CMAKE_SOURCE_DIR}/python/downloader/downloader.py"
)
add_dependencies(mediatool_integration_tests mediatool-core)
gtest_discover_tests(mediatool_integration_tests)

add_executable(mediatool_stress_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/stress/JobManagerStressTest.cpp"
)
target_link_libraries(mediatool_stress_tests PRIVATE
    mtcore
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(mediatool_stress_tests)

