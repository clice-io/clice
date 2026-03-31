cmake_minimum_required(VERSION 3.28)
project(no_modules_plain_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(test STATIC plain.cpp)
