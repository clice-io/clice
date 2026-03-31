cmake_minimum_required(VERSION 3.20)
project(dotted_module_name CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES io.cppm app.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
