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
    set(CLICE_SYMBOL_NAME "clice.pdb")
else()
    set(CLICE_ARCHIVE_EXT ".tar.gz")
    # The main archive stays .tar.gz for downloader compatibility; the symbol
    # archive is new enough to pick xz.
    set(CLICE_SYMBOL_ARCHIVE_EXT ".tar.xz")
    if(APPLE)
        set(CLICE_SYMBOL_NAME "clice.dSYM")
    else()
        set(CLICE_SYMBOL_NAME "clice.debug")
    endif()
    # Not REQUIRED: manual builds outside the pixi env may lack llvm-tools;
    # they only lose the clice-pack-symbol target below.
    find_program(CLICE_GSYMUTIL llvm-gsymutil)
endif()

if(WIN32)
    # The PDB already lives outside the binary; the "stripped" copy is plain.
    add_custom_target(clice-strip
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/stripped"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PDB_FILE:clice>"
            "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_STRIPPED}"
        DEPENDS clice
        COMMENT "Collecting PDB for clice"
    )
elseif(APPLE)
    add_custom_target(clice-strip
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/stripped"
        COMMAND dsymutil "$<TARGET_FILE:clice>" -o "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_STRIPPED}"
        COMMAND strip -x "${CLICE_STRIPPED}"
        DEPENDS clice
        COMMENT "Extracting dSYM and stripping clice"
    )
else()
    add_custom_target(clice-strip
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/stripped"
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug "$<TARGET_FILE:clice>" "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_STRIPPED}"
        COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded "${CLICE_STRIPPED}"
        COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink="${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "${CLICE_STRIPPED}"
        DEPENDS clice
        COMMENT "Extracting debug symbols and stripping clice"
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
        -DOUTPUT="${PROJECT_BINARY_DIR}/clice${CLICE_ARCHIVE_EXT}"
        -DWORK_DIR="${CLICE_PACK_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
    COMMENT "Packaging clice distribution"
)

# The released symbol package carries GSYM, not DWARF: it keeps everything
# crash symbolization needs (functions, lines, inline chains) at ~1/10 the
# size. The full DWARF stays in ${CLICE_SYMBOL_DIR} for CI to publish as a
# workflow artifact. Windows has no GSYM path and ships the PDB directly.
if(NOT WIN32 AND NOT CLICE_GSYMUTIL)
    message(STATUS "llvm-gsymutil not found: clice-pack-symbol target disabled")
    return()
endif()

if(WIN32)
    set(CLICE_PACK_SYMBOL_CMD ${CMAKE_COMMAND} -E copy
        "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "${CLICE_SYMBOL_DIR}/pack/")
else()
    if(APPLE)
        set(CLICE_GSYM_INPUT
            "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}/Contents/Resources/DWARF/clice")
    else()
        set(CLICE_GSYM_INPUT "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}")
    endif()
    # --merged-functions: ICF folds identical functions onto one address
    # range; without it only one of the folded names survives conversion.
    # --quiet: the same folding trips thousands of benign line-table
    # diagnostics that would otherwise drown the CI log.
    set(CLICE_PACK_SYMBOL_CMD ${CLICE_GSYMUTIL} --convert "${CLICE_GSYM_INPUT}"
        --merged-functions --quiet --out-file "${CLICE_SYMBOL_DIR}/pack/clice.gsym")
endif()

add_custom_target(clice-pack-symbol
    DEPENDS clice-strip
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_SYMBOL_DIR}/pack"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/pack"
    COMMAND ${CLICE_PACK_SYMBOL_CMD}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT="${PROJECT_BINARY_DIR}/clice-symbol${CLICE_SYMBOL_ARCHIVE_EXT}"
        -DWORK_DIR="${CLICE_SYMBOL_DIR}/pack"
        -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
    COMMENT "Packaging clice debug symbols"
)
