# A package exporting a C++ module, built and installed on its own: the
# project consumes it from where it is installed (COMPAT_PACKAGE).
cmake_minimum_required(VERSION 3.28)
project(compat_package CXX)

add_library(compat_package STATIC)
target_sources(compat_package PUBLIC FILE_SET CXX_MODULES FILES package.cppm)
target_include_directories(compat_package PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")
target_compile_features(compat_package PUBLIC cxx_std_20)

install(TARGETS compat_package EXPORT compat_package FILE_SET CXX_MODULES DESTINATION modules)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT compat_package NAMESPACE compat:: DESTINATION lib/cmake/compat_package
  FILE compat_packageConfig.cmake CXX_MODULES_DIRECTORY modules)
