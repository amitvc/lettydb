cmake_minimum_required(VERSION 3.22)
project(lettydb)

set(CMAKE_CXX_STANDARD 17)

# Set debug flags when in Debug mode
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# Add dependencies via FetchContent
# Skip network checks after first successful download
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_Declare(
    replxx
    GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
    GIT_TAG release-0.0.4
)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_Declare(
    tabulate
    GIT_REPOSITORY https://github.com/p-ranav/tabulate.git
    GIT_TAG v1.5
    DOWNLOAD_EXTRACT_TIMESTAMP true
)
FetchContent_MakeAvailable(spdlog replxx json tabulate)

file(GLOB_RECURSE DB_SOURCES "src/*.cpp")
file(GLOB_RECURSE DB_HEADERS "src/*.h")

# Remove main.cpp from library sources
list(REMOVE_ITEM DB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(lettydb ${DB_SOURCES} ${DB_HEADERS})
target_link_libraries(lettydb spdlog::spdlog replxx::replxx nlohmann_json::nlohmann_json tabulate::tabulate)

# Create executable
add_executable(lettydb_cli src/main.cpp)
target_link_libraries(lettydb_cli lettydb)


target_include_directories(lettydb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)


# Documentation with Doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
    # Set Doxygen configuration options
    set(DOXYGEN_PROJECT_NAME "LettyDB")
    set(DOXYGEN_PROJECT_BRIEF "A SQL-compliant database written in C++")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
    set(DOXYGEN_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_EXTRACT_ALL YES)
    set(DOXYGEN_EXTRACT_PRIVATE YES)
    set(DOXYGEN_EXTRACT_STATIC YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_HTML_OUTPUT "html")
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(DOXYGEN_FILE_PATTERNS "*.h *.hpp *.cpp *.cc *.cxx")
    set(DOXYGEN_EXCLUDE_PATTERNS "*/build/* */cmake-build-*/* */tests/*")
    set(DOXYGEN_COLLABORATION_GRAPH YES)
    set(DOXYGEN_HAVE_DOT YES)
    set(DOXYGEN_UML_LOOK YES)
    set(DOXYGEN_TEMPLATE_RELATIONS YES)
    set(DOXYGEN_INCLUDE_GRAPH YES)
    set(DOXYGEN_INCLUDED_BY_GRAPH YES)
    set(DOXYGEN_CALL_GRAPH YES)
    set(DOXYGEN_CALLER_GRAPH YES)

    # Configure Doxyfile
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    
    # Create Doxygen target
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
    
    message(STATUS "Doxygen found: ${DOXYGEN_EXECUTABLE}")
    message(STATUS "Documentation will be generated in: ${DOXYGEN_OUTPUT_DIRECTORY}")
    message(STATUS "Run 'make docs' or 'cmake --build . --target docs' to generate documentation")
else()
    message(WARNING "Doxygen not found - documentation target will not be available")
endif()

enable_testing()
add_subdirectory(tests)