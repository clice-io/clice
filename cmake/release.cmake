include_guard()

set(CLICE_PACK_DIR "${PROJECT_BINARY_DIR}/pack")
set(CLICE_SYMBOL_DIR "${PROJECT_BINARY_DIR}/pack-symbol")

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
    find_program(CLICE_GSYMUTIL llvm-gsymutil REQUIRED)
endif()

if(WIN32)
    add_custom_target(clice-strip ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PDB_FILE:clice>"
            "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        DEPENDS clice
        COMMENT "Collecting PDB for clice"
    )
elseif(APPLE)
    add_custom_target(clice-strip ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
        COMMAND dsymutil "$<TARGET_FILE:clice>" -o "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND strip -x "$<TARGET_FILE:clice>"
        DEPENDS clice
        COMMENT "Extracting dSYM and stripping clice"
    )
else()
    add_custom_target(clice-strip ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug "$<TARGET_FILE:clice>" "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
        COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded "$<TARGET_FILE:clice>"
        COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink="${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "$<TARGET_FILE:clice>"
        DEPENDS clice
        COMMENT "Extracting debug symbols and stripping clice"
    )
endif()

add_custom_target(clice-pack ALL
    DEPENDS clice-strip copy_clang_resource
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_PACK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_PACK_DIR}/clice/bin"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_PACK_DIR}/clice/bin/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LLVM_INSTALL_PATH}/lib/clang" "${CLICE_PACK_DIR}/clice/lib/clang"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/docs/clice.toml" "${CLICE_PACK_DIR}/clice/"
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

add_custom_target(clice-pack-symbol ALL
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
