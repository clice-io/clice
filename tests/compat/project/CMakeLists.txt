cmake_minimum_required(VERSION 3.20)
project(compat C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(compat src/main.cpp src/util.c)
target_include_directories(compat PRIVATE include)
target_compile_definitions(compat PRIVATE COMPAT_DEFINE=1)

if(COMPAT_PCH)
  target_precompile_headers(compat PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:<string$<ANGLE-R>;<vector$<ANGLE-R>>")
endif()

if(COMPAT_MODULES)
  add_library(compat_modules STATIC src/use_greeting.cpp)
  target_sources(compat_modules PUBLIC FILE_SET CXX_MODULES FILES src/greeting.cppm)
  target_compile_features(compat_modules PUBLIC cxx_std_20)
  set_target_properties(compat_modules PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

if(COMPAT_IMPORT_STD)
  add_library(compat_std STATIC std/use_std.cpp)
  target_compile_features(compat_std PUBLIC cxx_std_23)
  set_target_properties(compat_std PROPERTIES CXX_SCAN_FOR_MODULES ON)

  # A named module importing `std` in its primary interface, its partition
  # and its implementation unit, imported by another target beside `std`.
  add_library(compat_shapes STATIC std/shapes.cpp)
  target_sources(compat_shapes PUBLIC FILE_SET CXX_MODULES FILES std/shapes.cppm std/geometry.cppm)
  target_compile_features(compat_shapes PUBLIC cxx_std_23)
  add_library(compat_std_users STATIC std/use_shapes.cpp std/mixed.cpp std/use_compat.cpp)
  target_link_libraries(compat_std_users PRIVATE compat_shapes)
  set_target_properties(compat_shapes compat_std_users PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# A module unit and an importer of `std` in a target with a precompiled
# header, which CMake hands both (#1078). Only cl builds it: GCC 15 rejects
# the header ahead of the module declaration, and clang 18 finds libc++'s
# definitions in the header and in `std` differ.
if(COMPAT_STD_PCH)
  add_library(compat_std_pch STATIC std/pch_user.cpp)
  target_sources(compat_std_pch PUBLIC FILE_SET CXX_MODULES FILES std/pch_unit.cppm)
  target_precompile_headers(compat_std_pch PRIVATE <vector>)
  target_compile_features(compat_std_pch PUBLIC cxx_std_23)
  set_target_properties(compat_std_pch PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# An importer of `std` with other language options: CMake builds the
# standard library's module again for it (`@cmake_cxx_std@synth_0`), a
# second database entry of the module's source.
if(COMPAT_STD_PTHREAD)
  add_library(compat_std_threads STATIC std/use_std_threads.cpp)
  target_compile_features(compat_std_threads PUBLIC cxx_std_23)
  target_compile_options(compat_std_threads PUBLIC -pthread)
  set_target_properties(compat_std_threads PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

if(COMPAT_SYCL)
  add_library(compat_sycl STATIC sycl/kernel.cpp)
  target_include_directories(compat_sycl PRIVATE include)
  target_compile_options(compat_sycl PRIVATE -fsycl)
endif()

if(COMPAT_CUDA)
  enable_language(CUDA)
  add_library(compat_cuda STATIC cuda/kernel.cu)
  target_include_directories(compat_cuda PRIVATE include)
  target_compile_definitions(compat_cuda PRIVATE COMPAT_DEFINE=1)
endif()
