if(OUTPUT MATCHES "\\.tar\\.gz$")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar czf "${OUTPUT}" .
        WORKING_DIRECTORY "${WORK_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )
elseif(OUTPUT MATCHES "\\.tar\\.xz$")
    # cmake -E tar's built-in xz is single-threaded and dominates packaging
    # time on large debug-info trees; pipe system tar through xz -T0 instead.
    # This branch is only reached on non-Windows, where both tools exist.
    execute_process(
        COMMAND tar cf - .
        COMMAND xz -T0 -c
        OUTPUT_FILE "${OUTPUT}"
        WORKING_DIRECTORY "${WORK_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )
elseif(OUTPUT MATCHES "\\.zip$")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar cf "${OUTPUT}" --format=zip .
        WORKING_DIRECTORY "${WORK_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )
else()
    message(FATAL_ERROR "Unsupported archive format: ${OUTPUT}")
endif()

message(STATUS "Created: ${OUTPUT}")
