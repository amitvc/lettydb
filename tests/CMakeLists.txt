cmake_minimum_required(VERSION 3.22)
project(lettydb)

set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP true
)

# Make the gtest and gtest_main targets available
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE DB_SOURCES "*.cpp")
file(GLOB_RECURSE DB_HEADERS "*.h")

add_executable(lettydb_test ${DB_SOURCES} ${DB_HEADERS})

target_link_libraries(lettydb_test PRIVATE lettydb
        gtest
        gmock
        gmock_main
)
target_include_directories(lettydb_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(lettydb_test)