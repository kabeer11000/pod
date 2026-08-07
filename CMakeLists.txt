cmake_minimum_required(VERSION 3.15)
project(MyRealtimeDB CXX)

# Require C++20 for modern features
set(CMAKE_CXX_STANDARD 20)

# Find the packages you just added
find_package(leveldb CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(uWebSockets CONFIG REQUIRED)

# Create your main executable
add_executable(mydb main.cpp)

# Link the libraries to your executable
target_link_libraries(mydb PRIVATE 
    leveldb::leveldb 
    nlohmann_json::nlohmann_json 
    uWebSockets::uWebSockets
)