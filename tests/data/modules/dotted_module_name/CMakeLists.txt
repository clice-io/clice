cmake_minimum_required(VERSION 3.28)
project(dotted_module_name LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test)
target_sources(test
    PUBLIC
    FILE_SET CXX_MODULES
    FILES io.cppm app.cppm
)

