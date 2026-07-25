# TODO(clang23): DELETE this whole file when the toolchain reaches clang 23.
#
# clice-local workaround for llvm/llvm-project#174858 (fixed upstream by
# PR#184287, clang 23 only, no 22.x backport): clang 22's module serializer
# demotes variable definitions inside basic_format_string to declarations, so
# any TU that sees std::format through a BMI eagerly re-instantiates
# __compile_time_handle::__enable for phantom overload candidates (e.g. the
# wide-char format_string of a char-only call) and hard-errors on
# formatter<T, wchar_t>'s deleted default constructor.
#
# The patch guards __enable with a requires-check so those never-called
# phantom instantiations become no-ops; genuinely formattable combinations
# are untouched. It rewrites the libc++ header INSIDE the pixi environment —
# the environment is project-owned and disposable (reinstall restores the
# pristine header), nothing outside the repo checkout is affected.
function(clice_patch_libcxx_format)
    get_filename_component(toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(header "${toolchain_bin}/../include/c++/v1/__format/format_functions.h")
    if(NOT EXISTS "${header}")
        return()  # not a libc++ toolchain (e.g. Linux libstdc++): nothing to do
    endif()

    file(READ "${header}" content)
    if(content MATCHES "clice-libcxx-patch")
        message(STATUS "libc++ format_functions.h already patched (llvm#174858 workaround)")
        return()
    endif()

    set(original
"  template <class _Tp>
  _LIBCPP_HIDE_FROM_ABI constexpr void __enable() {
    __parse_ = [](basic_format_parse_context<_CharT>& __ctx) {
      formatter<_Tp, _CharT> __f;
      __ctx.advance_to(__f.parse(__ctx));
    };
  }")
    set(patched
"  template <class _Tp>
  _LIBCPP_HIDE_FROM_ABI constexpr void __enable() {
    // clice-libcxx-patch: guard phantom module re-instantiations (llvm#174858).
    if constexpr (requires(basic_format_parse_context<_CharT>& __pc) { formatter<_Tp, _CharT>{}.parse(__pc); }) {
    __parse_ = [](basic_format_parse_context<_CharT>& __ctx) {
      formatter<_Tp, _CharT> __f;
      __ctx.advance_to(__f.parse(__ctx));
    };
    }
  }")

    string(FIND "${content}" "${original}" pos)
    if(pos EQUAL -1)
        message(WARNING
            "libc++ format_functions.h did not match the llvm#174858 patch anchor; "
            "libc++ version drift? Building unpatched — macOS module builds may fail.")
        return()
    endif()

    string(REPLACE "${original}" "${patched}" content "${content}")
    file(WRITE "${header}" "${content}")
    message(STATUS "Patched libc++ format_functions.h (llvm#174858 workaround, TODO(clang23) remove)")
endfunction()

clice_patch_libcxx_format()
