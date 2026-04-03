cmake_minimum_required(VERSION 3.28)

project(clice_vscode_cmake_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sample_greeting greeting.cc)
target_include_directories(sample_greeting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(sample_app main.cc)
target_link_libraries(sample_app PRIVATE sample_greeting)

set(SAMPLE_MODULES_SUPPORTED OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    set(SAMPLE_MODULES_SUPPORTED ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
    set(SAMPLE_MODULES_SUPPORTED ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.34)
    set(SAMPLE_MODULES_SUPPORTED ON)
endif()

if(SAMPLE_MODULES_SUPPORTED)
    add_executable(sample_module_app)
    target_sources(sample_module_app PRIVATE main_module.cc)
    target_sources(sample_module_app
        PRIVATE
        FILE_SET CXX_MODULES
        FILES greeting_module.cppm
    )
else()
    message(STATUS
        "Skipping sample_module_app because the active compiler lacks "
        "CMake C++20 module scanning support. Use Clang >= 16, GCC >= 14, "
        "or MSVC 19.34+ to enable it."
    )
endif()
