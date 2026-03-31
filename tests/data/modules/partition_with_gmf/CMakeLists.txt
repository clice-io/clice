cmake_minimum_required(VERSION 3.20)
project(partition_with_gmf CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES part_cfg.cppm cfg.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
target_include_directories(test PRIVATE ${CMAKE_SOURCE_DIR})
