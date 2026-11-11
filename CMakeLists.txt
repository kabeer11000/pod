cmake_minimum_required(VERSION 3.15)
project(Pod CXX)

# Tell CMake to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Require C++23 for modern features (std::expected)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the packages you just added
find_package(leveldb CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers)
find_package(Threads REQUIRED)

# Automatically find ALL .cpp files inside the src/ directory and its subfolders
file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)
# main.cpp is the only translation unit not shared with the tests
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")

# Everything except main() lives in a static library shared by the server
# executable and the test suite.
add_library(pod_core STATIC ${PROJECT_SOURCES})

# THE MAGIC LINE: Tell CMake that 'src' is the root folder for #includes
target_include_directories(pod_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link the libraries to your executable
target_link_libraries(pod_core PUBLIC
    leveldb::leveldb
    nlohmann_json::nlohmann_json
    unofficial::uwebsockets::uwebsockets
    Boost::headers
    Threads::Threads
)

add_executable(pod src/main.cpp)
target_link_libraries(pod PRIVATE pod_core)

# Unit tests: cmake --build build --target pod_tests && ./build/pod_tests
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/unit/*.cpp")
add_executable(pod_tests EXCLUDE_FROM_ALL ${TEST_SOURCES})
target_link_libraries(pod_tests PRIVATE pod_core)
