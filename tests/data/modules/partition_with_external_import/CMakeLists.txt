cmake_minimum_required(VERSION 3.28)
project(partition_with_external_import LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test)
target_sources(test
    PUBLIC
    FILE_SET CXX_MODULES
    FILES ext.cppm part.cppm app.cppm
)

