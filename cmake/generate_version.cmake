# Build-time script: regenerate the version header from git describe.
#
# Runs on every build so the embedded version tracks the checked-out
# commit, not the last configure. configure_file only rewrites the output
# when the content changed, so dependents do not rebuild spuriously.
#
# Inputs: SOURCE_DIR, FALLBACK_VERSION, TEMPLATE, OUTPUT_FILE.

execute_process(
    COMMAND git -C "${SOURCE_DIR}" describe --tags --always --dirty
    OUTPUT_VARIABLE CLICE_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE CLICE_GIT_RESULT
)

if(NOT CLICE_GIT_RESULT EQUAL 0 OR CLICE_GIT_DESCRIBE STREQUAL "")
    # Not a git checkout (e.g. a source tarball): base version only.
    set(CLICE_VERSION_STRING "${FALLBACK_VERSION}")
elseif(CLICE_GIT_DESCRIBE MATCHES "^v")
    # Tag-relative describe; tags are named vX.Y.Z, strip the prefix.
    string(SUBSTRING "${CLICE_GIT_DESCRIBE}" 1 -1 CLICE_VERSION_STRING)
else()
    # Repository without a reachable tag: describe is a bare commit hash.
    set(CLICE_VERSION_STRING "${FALLBACK_VERSION}+g${CLICE_GIT_DESCRIBE}")
endif()

configure_file("${TEMPLATE}" "${OUTPUT_FILE}" @ONLY)
