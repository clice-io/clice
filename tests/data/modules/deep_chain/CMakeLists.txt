cmake_minimum_required(VERSION 3.28)
project(deep_chain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test)
target_sources(test
    PUBLIC
    FILE_SET CXX_MODULES
    FILES
    m1.cppm
    m2.cppm
    m3.cppm
    m4.cppm
    m5.cppm
)
