cmake_minimum_required(VERSION 3.20)
project(compat C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(compat src/main.cpp src/util.c)
target_include_directories(compat PRIVATE include)
target_compile_definitions(compat PRIVATE COMPAT_DEFINE=1)
