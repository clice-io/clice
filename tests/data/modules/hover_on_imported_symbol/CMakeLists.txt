cmake_minimum_required(VERSION 3.20)
project(hover_on_imported_symbol CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES defs.cppm use.cpp)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
