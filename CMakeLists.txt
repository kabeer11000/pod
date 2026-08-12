cmake_minimum_required(VERSION 3.15)
project(Pod CXX)

# Tell CMake to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Require C++23 for modern features
set(CMAKE_CXX_STANDARD 23)

# Find the packages you just added
find_package(leveldb CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers)

# Automatically find ALL .cpp files inside the src/ directory and its subfolders
file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)

# Create your main executable
add_executable(pod ${PROJECT_SOURCES})

# THE MAGIC LINE: Tell CMake that 'src' is the root folder for #includes
target_include_directories(pod PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link the libraries to your executable
target_link_libraries(pod PRIVATE
    leveldb::leveldb
    nlohmann_json::nlohmann_json
    unofficial::uwebsockets::uwebsockets
    Boost::headers
)
