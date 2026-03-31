cmake_minimum_required(VERSION 3.28)
project(module_implementation_unit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test)
target_sources(test
    PUBLIC
    FILE_SET CXX_MODULES
    FILES
    greeter.cppm
    greeter_impl.cpp
)
