# In LLVM 22 `--quiet` does not cover llvm-gsymutil's per-DIE warnings
# ("duplicate line table detected", one DIE dump each); on the macOS dSYM
# they add up to gigabytes, and streaming them through the CI runner's log
# is what made the packaging step take 25 minutes instead of seconds.

execute_process(
    COMMAND "${GSYMUTIL}" --convert "${INPUT}" --merged-functions --quiet --out-file "${OUTPUT}"
    OUTPUT_FILE "${LOG}"
    ERROR_FILE "${LOG}"
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    set(tail "")
    if(EXISTS "${LOG}")
        file(SIZE "${LOG}" log_size)
        math(EXPR offset "${log_size} - 4000")
        if(offset LESS 0)
            set(offset 0)
        endif()
        file(READ "${LOG}" tail OFFSET ${offset})
    endif()
    message(FATAL_ERROR "llvm-gsymutil failed (${result}); end of ${LOG}:\n${tail}")
endif()

file(SIZE "${LOG}" log_size)
message(STATUS "Created: ${OUTPUT} (${log_size} bytes of diagnostics in ${LOG})")
