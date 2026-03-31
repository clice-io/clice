include_guard()

set(CLICE_PACK_DIR "${PROJECT_BINARY_DIR}/pack")
set(CLICE_SYMBOL_DIR "${PROJECT_BINARY_DIR}/pack-symbol")

if(WIN32)
    set(CLICE_ARCHIVE_EXT ".zip")
    set(CLICE_SYMBOL_NAME "clice.pdb")
else()
    set(CLICE_ARCHIVE_EXT ".tar.gz")
    if(APPLE)
        set(CLICE_SYMBOL_NAME "clice.dSYM")
    else()
        set(CLICE_SYMBOL_NAME "clice.debug")
    endif()
endif()

if(WIN32)
    add_custom_target(clice-strip ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
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

add_custom_target(clice-pack-symbol ALL
    DEPENDS clice-strip
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_SYMBOL_DIR}/pack"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/pack"
    COMMAND ${CMAKE_COMMAND} -E copy "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "${CLICE_SYMBOL_DIR}/pack/"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT="${PROJECT_BINARY_DIR}/clice-symbol${CLICE_ARCHIVE_EXT}"
        -DWORK_DIR="${CLICE_SYMBOL_DIR}/pack"
        -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
    COMMENT "Packaging clice debug symbols"
)
