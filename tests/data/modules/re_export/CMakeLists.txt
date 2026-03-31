cmake_minimum_required(VERSION 3.20)
project(re_export CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES core.cppm wrapper.cppm user.cppm)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
add_library(test OBJECT ${SOURCES})
