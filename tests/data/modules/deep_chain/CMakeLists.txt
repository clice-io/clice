cmake_minimum_required(VERSION 3.20)
project(deep_chain CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES m1.cppm m2.cppm m3.cppm m4.cppm m5.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
