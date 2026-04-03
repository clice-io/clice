cmake_minimum_required(VERSION 3.16)

project(clice_vscode_cmake_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sample_greeting greeting.cc)
target_include_directories(sample_greeting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(sample_app main.cc)
target_link_libraries(sample_app PRIVATE sample_greeting)
