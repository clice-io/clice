cmake_minimum_required(VERSION 3.28)
project(global_module_fragment LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test)
target_sources(test
    PUBLIC
    FILE_SET CXX_MODULES
    FILES
    gmf.cppm
)
target_include_directories(test PRIVATE ${CMAKE_SOURCE_DIR})
