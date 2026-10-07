cmake_minimum_required(VERSION 3.28)
project(internal_partitions LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test)
target_sources(test
    PRIVATE impl.cpp main.cpp
    PUBLIC
    FILE_SET CXX_MODULES
    FILES detail.cppm util.cppm api.cppm lib.cppm
)
