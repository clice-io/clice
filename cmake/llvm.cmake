include_guard()

# Canonical target triple: the explicit CLICE_TARGET_TRIPLE for cross
# builds, composed from the host otherwise. This exact spelling names the
# prebuilt LLVM archives and the clice release assets.
function(clice_target_triple OUT_VAR)
    if(DEFINED CLICE_TARGET_TRIPLE)
        set(${OUT_VAR} "${CLICE_TARGET_TRIPLE}" PARENT_SCOPE)
        return()
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(_ARCH "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x64")
        set(_ARCH "x86_64")
    else()
        message(FATAL_ERROR "Unsupported processor: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(MINGW)
        set(${OUT_VAR} "${_ARCH}-w64-mingw32" PARENT_SCOPE)
    elseif(WIN32)
        set(${OUT_VAR} "${_ARCH}-pc-windows-msvc" PARENT_SCOPE)
    elseif(APPLE)
        set(${OUT_VAR} "${_ARCH}-apple-darwin" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${_ARCH}-unknown-linux-gnu" PARENT_SCOPE)
    endif()
endfunction()

function(_download_llvm LLVM_VERSION)
    clice_target_triple(_TRIPLE)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_MODE "debug")
    else()
        set(_MODE "releasedbg")
    endif()

    set(_SUFFIX "")
    if(CLICE_ENABLE_LTO)
        string(APPEND _SUFFIX "-lto")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32)
        string(APPEND _SUFFIX "-asan")
    endif()

    set(_FILENAME "${_TRIPLE}.${_MODE}${_SUFFIX}.tar.xz")
    string(REPLACE "+" "%2B" _URL_VERSION "${LLVM_VERSION}")
    # A release like 23.1.1+r1 is not a CMake version; CPM hands VERSION to
    # find_package when local packages are enabled.
    string(REGEX REPLACE "\\+.*" "" _CMAKE_VERSION "${LLVM_VERSION}")

    CPMAddPackage(
        NAME llvm_prebuilt
        VERSION ${_CMAKE_VERSION}
        URL "https://github.com/clice-io/clice-llvm/releases/download/${_URL_VERSION}/${_FILENAME}"
        DOWNLOAD_ONLY YES
    )

    if(NOT EXISTS "${llvm_prebuilt_SOURCE_DIR}/lib/cmake/llvm")
        # An interrupted download leaves a directory in the cache that CPM
        # would keep treating as the package; a directory the developer
        # pointed at through CPM_llvm_prebuilt_SOURCE is theirs to fix.
        cmake_path(IS_PREFIX CPM_SOURCE_CACHE "${llvm_prebuilt_SOURCE_DIR}" NORMALIZE _in_cache)
        if(_in_cache)
            file(REMOVE_RECURSE "${llvm_prebuilt_SOURCE_DIR}")
            message(FATAL_ERROR
                "The LLVM archive at ${llvm_prebuilt_SOURCE_DIR} was incomplete and has been "
                "removed; run the configure again.")
        endif()
        message(FATAL_ERROR
            "No LLVM install at ${llvm_prebuilt_SOURCE_DIR}: lib/cmake/llvm is missing.")
    endif()

    set(LLVM_INSTALL_PATH "${llvm_prebuilt_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(setup_llvm LLVM_VERSION)
    if(DEFINED LLVM_INSTALL_PATH AND NOT LLVM_INSTALL_PATH STREQUAL "")
        get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)
        if(NOT EXISTS "${LLVM_INSTALL_PATH}/lib/cmake/llvm")
            # The path is cached below, so a wiped source cache would otherwise
            # keep every later configure of this build tree pointed at nothing;
            # a path given by hand is reported instead.
            cmake_path(IS_PREFIX CPM_SOURCE_CACHE "${LLVM_INSTALL_PATH}" NORMALIZE _in_cache)
            if(NOT _in_cache)
                message(FATAL_ERROR
                    "No LLVM install at ${LLVM_INSTALL_PATH}: lib/cmake/llvm is missing. Point "
                    "LLVM_INSTALL_PATH at an LLVM install, or unset it (-ULLVM_INSTALL_PATH) to "
                    "download the prebuilt one.")
            endif()
            message(STATUS "LLVM not found at ${LLVM_INSTALL_PATH}, downloading")
            unset(LLVM_INSTALL_PATH)
            unset(LLVM_INSTALL_PATH CACHE)
        endif()
    endif()

    if(NOT DEFINED LLVM_INSTALL_PATH OR LLVM_INSTALL_PATH STREQUAL "")
        if(CLICE_OFFLINE_BUILD)
            message(FATAL_ERROR "LLVM_INSTALL_PATH must be set in offline mode")
        endif()
        _download_llvm("${LLVM_VERSION}")
    endif()

    set(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" CACHE PATH "LLVM install" FORCE)

    find_package(LLVM REQUIRED CONFIG
        PATHS "${LLVM_INSTALL_PATH}/lib/cmake/llvm" NO_DEFAULT_PATH)
    find_package(Clang REQUIRED CONFIG
        PATHS "${LLVM_INSTALL_PATH}/lib/cmake/clang" NO_DEFAULT_PATH)

    _check_llvm_manifest("${LLVM_INSTALL_PATH}")

    # The package's libc++ is the standard library of everything in this
    # build, third-party dependencies and their configure checks included, so
    # the flags go into the global CMAKE_* variables (add_compile_options and
    # link_libraries do not reach try_compile). The archive itself goes into
    # the standard libraries, which CMake places after the objects and link
    # libraries: a linker that scans archives in command-line order (GNU ld,
    # ld64) would otherwise drop it before anything references it. try_compile
    # does not carry that variable on its own.
    _llvm_libcxx_flags("${LLVM_INSTALL_PATH}" _cxx_flags _link_flags _libcxx)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_cxx_flags}" PARENT_SCOPE)
    foreach(kind EXE SHARED MODULE)
        set(CMAKE_${kind}_LINKER_FLAGS "${CMAKE_${kind}_LINKER_FLAGS} ${_link_flags}" PARENT_SCOPE)
    endforeach()
    set(CMAKE_CXX_STANDARD_LIBRARIES "${_libcxx} ${CMAKE_CXX_STANDARD_LIBRARIES}" PARENT_SCOPE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_CXX_STANDARD_LIBRARIES)
    set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES "${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES}" PARENT_SCOPE)

    llvm_map_components_to_libnames(LLVM_RESOLVED
        support frontendopenmp option targetparser)

    # The package ships the transitive closure of this list (COMPONENTS in
    # scripts/build-llvm.py); a library added here goes there as well.
    add_library(llvm-libs INTERFACE IMPORTED)
    target_link_libraries(llvm-libs INTERFACE
        ${LLVM_RESOLVED}
        clangAST clangASTMatchers clangBasic clangDriver
        clangFormat clangFrontend clangLex clangOptions clangSema clangSerialization
        clangToolingInclusionsStdlib
        clangTidy clangTidyUtils
        clangTidyAbseilModule clangTidyAlteraModule clangTidyAndroidModule
        clangTidyBoostModule clangTidyBugproneModule clangTidyCERTModule
        clangTidyConcurrencyModule clangTidyCppCoreGuidelinesModule
        clangTidyDarwinModule clangTidyFuchsiaModule
        clangTidyGoogleModule clangTidyLinuxKernelModule
        clangTidyLLVMModule clangTidyLLVMLibcModule clangTidyMiscModule
        clangTidyModernizeModule clangTidyObjCModule
        clangTidyOpenMPModule clangTidyPerformanceModule
        clangTidyPortabilityModule clangTidyReadabilityModule
        clangTidyZirconModule
        clangTooling clangToolingCore
        clangToolingInclusions clangToolingInclusionsStdlib clangToolingSyntax
    )

    # A package with the X86 MC layer lets clang parse MS-style __asm blocks;
    # clice registers the target at startup (src/clice.cc).
    if(TARGET LLVMX86AsmParser)
        target_link_libraries(llvm-libs INTERFACE LLVMX86AsmParser LLVMX86Desc LLVMX86Info)
        target_compile_definitions(llvm-libs INTERFACE CLICE_X86_MC=1)
    endif()

    target_include_directories(llvm-libs SYSTEM INTERFACE
        "${LLVM_INSTALL_PATH}/include")
    target_compile_definitions(llvm-libs INTERFACE CLANG_BUILD_STATIC=1)

    message(STATUS "LLVM ${LLVM_VERSION} at ${LLVM_INSTALL_PATH}")
endfunction()

# The archive records the toolchain it was built with. Every field checked
# here is one where a mismatch still links and then fails at runtime.
function(_check_llvm_manifest install_path)
    set(_manifest "${install_path}/lib/cmake/clice-llvm/config.cmake")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR
            "No clice-llvm manifest at ${_manifest}: this LLVM install predates the "
            "libc++ packages (23.1.1+r1). Point LLVM_INSTALL_PATH at a newer package, or unset it "
            "(-ULLVM_INSTALL_PATH) to download one.")
    endif()
    include("${_manifest}")
    if(NOT DEFINED CLICE_LLVM_LIBCXX_ABI_VERSION)
        message(FATAL_ERROR
            "The LLVM package at ${install_path} ships no libc++: it predates the "
            "23.1.1+r1 packages. Point LLVM_INSTALL_PATH at a newer package, or unset it "
            "(-ULLVM_INSTALL_PATH) to download one.")
    endif()
    clice_target_triple(_triple)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_expected_debug ON)
    else()
        set(_expected_debug OFF)
    endif()
    if(CLICE_LLVM_BUILD_TYPE STREQUAL "Debug")
        set(_package_debug ON)
    else()
        set(_package_debug OFF)
    endif()
    if(_expected_debug AND NOT WIN32)
        set(_expected_asan ON)
    else()
        set(_expected_asan OFF)
    endif()
    if(CLICE_ENABLE_LTO)
        set(_expected_lto ON)
    else()
        set(_expected_lto OFF)
    endif()

    set(_mismatch "")
    if(NOT CLICE_LLVM_TARGET_TRIPLE STREQUAL _triple)
        string(APPEND _mismatch "\n  target: package ${CLICE_LLVM_TARGET_TRIPLE}, this build ${_triple}")
    endif()
    if(NOT CLICE_LLVM_COMPILER_ID STREQUAL CMAKE_CXX_COMPILER_ID
            OR NOT CLICE_LLVM_COMPILER_VERSION VERSION_EQUAL CMAKE_CXX_COMPILER_VERSION)
        string(APPEND _mismatch "\n  compiler: package ${CLICE_LLVM_COMPILER_ID} "
            "${CLICE_LLVM_COMPILER_VERSION}, this build ${CMAKE_CXX_COMPILER_ID} "
            "${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    if(NOT _package_debug STREQUAL _expected_debug)
        string(APPEND _mismatch "\n  build type: package ${CLICE_LLVM_BUILD_TYPE}, "
            "this build ${CMAKE_BUILD_TYPE}")
    endif()
    if(NOT CLICE_LLVM_LTO STREQUAL _expected_lto)
        string(APPEND _mismatch "\n  LTO: package ${CLICE_LLVM_LTO}, this build ${_expected_lto}")
    endif()
    if(NOT CLICE_LLVM_ASAN STREQUAL _expected_asan)
        string(APPEND _mismatch "\n  ASan: package ${CLICE_LLVM_ASAN}, this build ${_expected_asan}")
    endif()
    if(NOT CLICE_LLVM_STDLIB STREQUAL "libc++")
        string(APPEND _mismatch "\n  standard library: package ${CLICE_LLVM_STDLIB}, this build libc++")
    endif()
    if(WIN32 AND NOT MINGW AND NOT CLICE_LLVM_MSVC_RUNTIME_LIBRARY STREQUAL CMAKE_MSVC_RUNTIME_LIBRARY)
        string(APPEND _mismatch "\n  MSVC runtime: package ${CLICE_LLVM_MSVC_RUNTIME_LIBRARY}, "
            "this build '${CMAKE_MSVC_RUNTIME_LIBRARY}'")
    endif()
    if(_mismatch)
        message(FATAL_ERROR "The LLVM package at ${install_path} does not match this build:${_mismatch}")
    endif()
endfunction()

# Compile and link flags that make the package's static libc++ the standard
# library. -nostdinc++ removes the host's C++ headers on Linux and macOS; on
# Windows the MSVC STL sits in the INCLUDE directories together with the C
# runtime headers, which -isystem precedes. There libc++.lib is a plain linker
# input rather than a /DEFAULTLIB: lld-link reads the command-line inputs and
# the objects' directive libraries (libcmt) before any /DEFAULTLIB, and libcmt
# also defines std::nothrow, so a default-library libc++ loses that symbol to
# libcmt and then duplicates it once new_helpers.cpp.obj is pulled in for
# __throw_bad_alloc. On the vcruntime ABI libc++ leaves std::set_new_handler to
# the MSVC STL (libcpmt), which nothing auto-links once its headers are
# shadowed; it duplicates libc++'s exception_ptr definitions, so it stays a
# default library, searched after everything else. Elsewhere the archive is
# also named outright: -stdlib=libc++ with a -L would still let a libc++.so
# from an earlier -L (LDFLAGS) win the link.
function(_llvm_libcxx_flags install_path cxx_flags_var link_flags_var libcxx_var)
    set(_include "${install_path}/include/c++/v1")
    set(_lib "${install_path}/lib")
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(${cxx_flags_var} "/clang:-isystem\"${_include}\"" PARENT_SCOPE)
        set(${link_flags_var} "/DEFAULTLIB:libcpmt.lib" PARENT_SCOPE)
        set(${libcxx_var} "\"${_lib}/libc++.lib\"" PARENT_SCOPE)
    elseif(WIN32 AND NOT MINGW)
        set(${cxx_flags_var} "-nostdinc++ -isystem \"${_include}\"" PARENT_SCOPE)
        set(${link_flags_var} "-Wl,/DEFAULTLIB:libcpmt.lib" PARENT_SCOPE)
        set(${libcxx_var} "\"${_lib}/libc++.lib\"" PARENT_SCOPE)
    else()
        set(${cxx_flags_var} "-nostdinc++ -isystem \"${_include}\"" PARENT_SCOPE)
        set(${link_flags_var} "-nostdlib++" PARENT_SCOPE)
        set(${libcxx_var} "\"${_lib}/libc++.a\"" PARENT_SCOPE)
    endif()
endfunction()
