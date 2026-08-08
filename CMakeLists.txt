cmake_minimum_required(VERSION 3.15)
project(Pod CXX)

# Tell CMake to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Require C++20 for modern features
set(CMAKE_CXX_STANDARD 20)

# Find the packages you just added
find_package(leveldb CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED) # Updated line
find_package(Boost REQUIRED COMPONENTS headers)

file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
    "main.cpp"
    "./*.cpp"
)
# Create your main executable
add_executable(pod ${PROJECT_SOURCES})

# Link the libraries to your executable
target_link_libraries(pod PRIVATE
    leveldb::leveldb
    nlohmann_json::nlohmann_json
    unofficial::uwebsockets::uwebsockets # Updated line
    Boost::headers
)
