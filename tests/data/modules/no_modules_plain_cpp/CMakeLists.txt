cmake_minimum_required(VERSION 3.20)
project(no_modules_plain_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(test OBJECT plain.cpp)
