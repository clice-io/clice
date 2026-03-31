cmake_minimum_required(VERSION 3.20)
project(partition_with_external_import CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES ext.cppm part.cppm app.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
