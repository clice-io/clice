include_guard(GLOBAL)

set(
    CLICE_BUILTIN_LIBRARY_MODULES
    "${CLICE_BUILTIN_LIBRARY_MODULES}"
    CACHE STRING
    "Semicolon-separated list of CMake modules that register extra builtin clice libraries"
)

function(clice_add_builtin_library)
    set(options)
    set(oneValueArgs NAME ENTRYPOINT)
    set(multiValueArgs SOURCES INCLUDE_DIRECTORIES LINK_LIBRARIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
    cmake_parse_arguments(CBL "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(CBL_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "clice_add_builtin_library got unexpected arguments: ${CBL_UNPARSED_ARGUMENTS}"
        )
    endif()

    if(NOT CBL_NAME)
        message(FATAL_ERROR "clice_add_builtin_library requires NAME")
    endif()

    if(NOT CBL_SOURCES)
        message(FATAL_ERROR "clice_add_builtin_library(${CBL_NAME}) requires SOURCES")
    endif()

    if(NOT CBL_ENTRYPOINT)
        message(FATAL_ERROR "clice_add_builtin_library(${CBL_NAME}) requires ENTRYPOINT")
    endif()

    if(NOT TARGET clice_builtin_api)
        message(
            FATAL_ERROR
            "clice_builtin_api must be defined before calling clice_add_builtin_library(${CBL_NAME})"
        )
    endif()

    set(target "clice_builtin_${CBL_NAME}")
    if(TARGET "${target}")
        message(FATAL_ERROR "builtin library target '${target}' already exists")
    endif()

    add_library("${target}" OBJECT)
    target_sources("${target}" PRIVATE ${CBL_SOURCES})
    target_link_libraries("${target}" PUBLIC clice_builtin_api)
    add_dependencies("${target}" generate_flatbuffers_schema generate_config)

    if(CBL_INCLUDE_DIRECTORIES)
        target_include_directories("${target}" PRIVATE ${CBL_INCLUDE_DIRECTORIES})
    endif()

    if(CBL_LINK_LIBRARIES)
        target_link_libraries("${target}" PUBLIC ${CBL_LINK_LIBRARIES})
    endif()

    if(CBL_COMPILE_DEFINITIONS)
        target_compile_definitions("${target}" PRIVATE ${CBL_COMPILE_DEFINITIONS})
    endif()

    if(CBL_COMPILE_OPTIONS)
        target_compile_options("${target}" PRIVATE ${CBL_COMPILE_OPTIONS})
    endif()

    set_property(GLOBAL APPEND PROPERTY CLICE_BUILTIN_LIBRARY_TARGETS "${target}")
    set_property(GLOBAL APPEND PROPERTY CLICE_BUILTIN_LIBRARY_ENTRYPOINTS "${CBL_ENTRYPOINT}")
endfunction()

function(clice_include_builtin_library_modules)
    foreach(module_path IN LISTS CLICE_BUILTIN_LIBRARY_MODULES)
        cmake_path(
            ABSOLUTE_PATH module_path
            BASE_DIRECTORY "${PROJECT_SOURCE_DIR}"
            NORMALIZE
            OUTPUT_VARIABLE module_abs_path
        )

        if(NOT EXISTS "${module_abs_path}")
            message(
                FATAL_ERROR
                "builtin library module '${module_path}' does not exist: ${module_abs_path}"
            )
        endif()

        include("${module_abs_path}")
    endforeach()
endfunction()

function(clice_finalize_builtin_libraries)
    set(options)
    set(oneValueArgs TARGET REGISTRATION_SOURCE)
    cmake_parse_arguments(CBL "${options}" "${oneValueArgs}" "" ${ARGN})

    if(CBL_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "clice_finalize_builtin_libraries got unexpected arguments: ${CBL_UNPARSED_ARGUMENTS}"
        )
    endif()

    if(NOT CBL_TARGET)
        message(FATAL_ERROR "clice_finalize_builtin_libraries requires TARGET")
    endif()

    if(NOT TARGET "${CBL_TARGET}")
        message(FATAL_ERROR "target '${CBL_TARGET}' does not exist")
    endif()

    if(NOT CBL_REGISTRATION_SOURCE)
        set(CBL_REGISTRATION_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin-libraries.cpp")
    endif()

    get_property(builtin_targets GLOBAL PROPERTY CLICE_BUILTIN_LIBRARY_TARGETS)
    get_property(builtin_entrypoints GLOBAL PROPERTY CLICE_BUILTIN_LIBRARY_ENTRYPOINTS)

    if(NOT builtin_targets)
        set(builtin_targets "")
    endif()

    if(NOT builtin_entrypoints)
        set(builtin_entrypoints "")
    endif()

    set(registration_source_content "#include \"Server/Plugin.h\"\n\nnamespace clice {\n\n")

    foreach(entrypoint IN LISTS builtin_entrypoints)
        string(APPEND registration_source_content "::clice::PluginInfo ${entrypoint}();\n")
    endforeach()

    string(APPEND registration_source_content "\nvoid register_builtin_server_plugins(ServerPluginBuilder& builder) {\n")

    foreach(entrypoint IN LISTS builtin_entrypoints)
        string(
            APPEND
            registration_source_content
            "    ${entrypoint}().register_server_callbacks(builder);\n"
        )
    endforeach()

    string(APPEND registration_source_content "}\n\n}  // namespace clice\n")

    get_filename_component(registration_source_dir "${CBL_REGISTRATION_SOURCE}" DIRECTORY)
    file(MAKE_DIRECTORY "${registration_source_dir}")
    file(CONFIGURE OUTPUT "${CBL_REGISTRATION_SOURCE}" CONTENT "${registration_source_content}" @ONLY)

    target_sources("${CBL_TARGET}" PRIVATE "${CBL_REGISTRATION_SOURCE}")

    if(builtin_targets)
        foreach(builtin_target IN LISTS builtin_targets)
            target_sources("${CBL_TARGET}" PRIVATE $<TARGET_OBJECTS:${builtin_target}>)
        endforeach()

        target_link_libraries("${CBL_TARGET}" PRIVATE ${builtin_targets})
    endif()
endfunction()
