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
