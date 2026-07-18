# Build-time script: regenerate the version header from git describe.
#
# Runs on every build so the embedded version tracks the checked-out
# commit, not the last configure. configure_file only rewrites the output
# when the content changed, so dependents do not rebuild spuriously.
#
# Inputs: SOURCE_DIR, FALLBACK_VERSION, TEMPLATE, OUTPUT_FILE.

# Guard against git's parent-directory repository discovery: a source
# tarball unpacked inside some other checkout must take the tarball
# fallback, not describe the surrounding repository. Trust git only when
# the discovered top-level is the source directory itself.
execute_process(
    COMMAND git -C "${SOURCE_DIR}" rev-parse --show-toplevel
    OUTPUT_VARIABLE CLICE_GIT_TOPLEVEL
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE CLICE_TOPLEVEL_RESULT
)

set(CLICE_GIT_RESULT 1)
if(CLICE_TOPLEVEL_RESULT EQUAL 0 AND NOT CLICE_GIT_TOPLEVEL STREQUAL "")
    file(REAL_PATH "${CLICE_GIT_TOPLEVEL}" CLICE_GIT_TOPLEVEL)
    file(REAL_PATH "${SOURCE_DIR}" CLICE_SOURCE_REAL)
    if(CLICE_GIT_TOPLEVEL STREQUAL CLICE_SOURCE_REAL)
        # A commit can carry several tags (a stable tag placed on a nightly's
        # commit); describe alone picks one nondeterministically, so prefer
        # the highest exact tag by version sort.
        execute_process(
            COMMAND git -C "${SOURCE_DIR}" tag --points-at HEAD --sort=-v:refname
            OUTPUT_VARIABLE CLICE_HEAD_TAGS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT CLICE_HEAD_TAGS STREQUAL "")
            string(REGEX REPLACE "\n.*" "" CLICE_GIT_DESCRIBE "${CLICE_HEAD_TAGS}")
            # Keep parity with describe --dirty: a modified tree must not
            # claim to be the pristine tagged artifact.
            execute_process(
                COMMAND git -C "${SOURCE_DIR}" diff-index --quiet HEAD --
                RESULT_VARIABLE CLICE_DIRTY_RESULT
                ERROR_QUIET
            )
            if(NOT CLICE_DIRTY_RESULT EQUAL 0)
                string(APPEND CLICE_GIT_DESCRIBE "-dirty")
            endif()
            set(CLICE_GIT_RESULT 0)
        else()
            execute_process(
                COMMAND git -C "${SOURCE_DIR}" describe --tags --always --dirty
                OUTPUT_VARIABLE CLICE_GIT_DESCRIBE
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE CLICE_GIT_RESULT
            )
        endif()
    endif()
endif()

if(NOT CLICE_GIT_RESULT EQUAL 0 OR CLICE_GIT_DESCRIBE STREQUAL "")
    # Not a git checkout of clice (e.g. a source tarball): base version only.
    set(CLICE_VERSION_STRING "${FALLBACK_VERSION}")
elseif(CLICE_GIT_DESCRIBE MATCHES "^[0-9a-f]+(-dirty)?$")
    # No tag reachable: describe degrades to a bare commit hash.
    set(CLICE_VERSION_STRING "${FALLBACK_VERSION}+g${CLICE_GIT_DESCRIBE}")
elseif(CLICE_GIT_DESCRIBE MATCHES "^v")
    # Tag-relative describe; tags are named vX.Y.Z, strip the prefix.
    string(SUBSTRING "${CLICE_GIT_DESCRIBE}" 1 -1 CLICE_VERSION_STRING)
else()
    # A tag without the v prefix: use the describe output verbatim.
    set(CLICE_VERSION_STRING "${CLICE_GIT_DESCRIBE}")
endif()

set(CLICE_TARGET_STRING "${TARGET_STRING}")

configure_file("${TEMPLATE}" "${OUTPUT_FILE}" @ONLY)
