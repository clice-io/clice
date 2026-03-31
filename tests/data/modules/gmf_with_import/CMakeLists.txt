cmake_minimum_required(VERSION 3.20)
project(gmf_with_import CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES base.cppm combined.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
target_include_directories(test PRIVATE ${CMAKE_SOURCE_DIR})
