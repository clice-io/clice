include_guard()

# Release packaging targets. They are not part of ALL: CI (and local
# packaging) invokes them explicitly after the test suites pass, against the
# very binary the tests ran — there is no separate release build. Stripping
# operates on a staged copy so the build-tree binary keeps its debug info.

set(CLICE_PACK_DIR "${PROJECT_BINARY_DIR}/pack")
set(CLICE_SYMBOL_DIR "${PROJECT_BINARY_DIR}/pack-symbol")
set(CLICE_STRIPPED "${CLICE_SYMBOL_DIR}/stripped/$<TARGET_FILE_NAME:clice>")

if(WIN32)
    set(CLICE_ARCHIVE_EXT ".zip")
    set(CLICE_SYMBOL_ARCHIVE_EXT ".zip")
else()
    set(CLICE_ARCHIVE_EXT ".tar.gz")
    # The main archive stays .tar.gz for downloader compatibility; the symbol
    # archive is new enough to pick xz.
    set(CLICE_SYMBOL_ARCHIVE_EXT ".tar.xz")
endif()
if(APPLE)
    set(CLICE_SYMBOL_NAME "clice.dSYM")
else()
    # DWARF on Windows as well: MinGW binaries carry it like ELF ones.
    set(CLICE_SYMBOL_NAME "clice.debug")
endif()
# Not REQUIRED: manual builds outside the pixi env may lack the LLVM tools;
# they only lose the clice-pack-symbol target below.
find_program(CLICE_GSYMUTIL llvm-gsymutil)

if(APPLE)
    add_custom_target(clice-strip
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/stripped"
        COMMAND dsymutil "$<TARGET_FILE:clice>" -o "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_STRIPPED}"
        COMMAND strip -x "${CLICE_STRIPPED}"
        DEPENDS clice
        COMMENT "Extracting dSYM and stripping clice"
        VERBATIM
    )
else()
    add_custom_target(clice-strip
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/stripped"
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug "$<TARGET_FILE:clice>" "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_STRIPPED}"
        COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded "${CLICE_STRIPPED}"
        COMMAND ${CMAKE_OBJCOPY} "--add-gnu-debuglink=${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "${CLICE_STRIPPED}"
        DEPENDS clice
        COMMENT "Extracting debug symbols and stripping clice"
        VERBATIM
    )
endif()

add_custom_target(clice-pack
    DEPENDS clice-strip copy_clang_resource
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_PACK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_PACK_DIR}/clice/bin"
    COMMAND ${CMAKE_COMMAND} -E copy "${CLICE_STRIPPED}" "${CLICE_PACK_DIR}/clice/bin/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LLVM_INSTALL_PATH}/lib/clang" "${CLICE_PACK_DIR}/clice/lib/clang"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/docs/clice.toml"
        "${PROJECT_SOURCE_DIR}/LICENSE" "${CLICE_PACK_DIR}/clice/"
    COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT=${PROJECT_BINARY_DIR}/clice${CLICE_ARCHIVE_EXT}"
        "-DWORK_DIR=${CLICE_PACK_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
    COMMENT "Packaging clice distribution"
    VERBATIM
)

# The released symbol package carries GSYM, not DWARF: it keeps everything
# crash symbolization needs (functions, lines, inline chains) at ~1/10 the
# size. The full DWARF stays in ${CLICE_SYMBOL_DIR} for CI to publish as a
# workflow artifact.
if(NOT CLICE_GSYMUTIL)
    message(STATUS "llvm-gsymutil not found: clice-pack-symbol target disabled")
    return()
endif()

if(APPLE)
    set(CLICE_GSYM_INPUT
        "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}/Contents/Resources/DWARF/clice")
else()
    set(CLICE_GSYM_INPUT "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}")
endif()
# --merged-functions: ICF folds identical functions onto one address
# range; without it only one of the folded names survives conversion.
# The same folding trips one line-table warning per folded DIE, which
# gsym.cmake keeps out of the build log.
set(CLICE_PACK_SYMBOL_CMD ${CMAKE_COMMAND}
    "-DGSYMUTIL=${CLICE_GSYMUTIL}"
    "-DINPUT=${CLICE_GSYM_INPUT}"
    "-DOUTPUT=${CLICE_SYMBOL_DIR}/pack/clice.gsym"
    "-DLOG=${CLICE_SYMBOL_DIR}/gsymutil.log"
    -P "${PROJECT_SOURCE_DIR}/cmake/gsym.cmake")

add_custom_target(clice-pack-symbol
    DEPENDS clice-strip
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_SYMBOL_DIR}/pack"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/pack"
    COMMAND ${CLICE_PACK_SYMBOL_CMD}
    COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT=${PROJECT_BINARY_DIR}/clice-symbol${CLICE_SYMBOL_ARCHIVE_EXT}"
        "-DWORK_DIR=${CLICE_SYMBOL_DIR}/pack"
        -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
    COMMENT "Packaging clice debug symbols"
    VERBATIM
)
