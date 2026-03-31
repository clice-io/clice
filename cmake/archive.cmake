if(OUTPUT MATCHES "\\.tar\\.gz$")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar czf "${OUTPUT}" .
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
