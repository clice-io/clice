cmake_minimum_required(VERSION 3.20)
project(circular_module_dependency CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES cycle_a.cppm cycle_b.cppm ok.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
