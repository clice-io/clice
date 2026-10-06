#pragma once
#  define _LIBCPP_VERSION 230102
#    define _NOEXCEPT noexcept
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_LIBC_LLVM_LIBC 0
#  define _LIBCPP_PREFERRED_OVERLOAD __attribute__((__enable_if__(true, "")))
#    define _LIBCPP_VISIBILITY(vis) __attribute__((__visibility__(vis)))
#  define _LIBCPP_HIDDEN _LIBCPP_VISIBILITY("hidden")
#    define _LIBCPP_NAMESPACE_VISIBILITY __attribute__((__type_visibility__("default")))
#  define _LIBCPP_EXCLUDE_FROM_EXPLICIT_INSTANTIATION __attribute__((__exclude_from_explicit_instantiation__))
#  define _LIBCPP_HARDENING_SIG n // "none"
#  define _LIBCPP_ASSERTION_SEMANTIC_SIG q
#  define _LIBCPP_EXCEPTIONS_SIG e
#define _LIBCPP_ODR_SIGNATURE                                                                                            _LIBCPP_CONCAT(                                                                                                            _LIBCPP_CONCAT(_LIBCPP_CONCAT(_LIBCPP_HARDENING_SIG, _LIBCPP_ASSERTION_SEMANTIC_SIG), _LIBCPP_EXCEPTIONS_SIG),         _LIBCPP_VERSION)
#  define _LIBCPP_HIDE_FROM_ABI                                                                                            _LIBCPP_HIDDEN _LIBCPP_EXCLUDE_FROM_EXPLICIT_INSTANTIATION                                                             __attribute__((__abi_tag__(_LIBCPP_TOSTRING(_LIBCPP_ODR_SIGNATURE))))
#  define _LIBCPP_USING_IF_EXISTS __attribute__((__using_if_exists__))
#  define _LIBCPP_PUSH_EXTENSION_DIAGNOSTICS
#  define _LIBCPP_POP_EXTENSION_DIAGNOSTICS
#  define _LIBCPP_PUSH_ABI_PRAGMA_DIAGNOSTICS
#  define _LIBCPP_POP_ABI_PRAGMA_DIAGNOSTICS
#define _LIBCPP_END_EXPLICIT_ABI_ANNOTATIONS                                                                             _LIBCPP_PUSH_ABI_PRAGMA_DIAGNOSTICS                                                                                    _Pragma(_LIBCPP_TOSTRING(clang attribute _LibcxxExplicitABIAnnotations.push(                                               __attribute__((__exclude_from_explicit_instantiation__,                                                                               __visibility__("hidden"),                                                                                              __abi_tag__(_LIBCPP_TOSTRING(_LIBCPP_ODR_SIGNATURE)))),                                                 apply_to = function))) _LIBCPP_POP_ABI_PRAGMA_DIAGNOSTICS
#define _LIBCPP_BEGIN_EXPLICIT_ABI_ANNOTATIONS _Pragma("clang attribute _LibcxxExplicitABIAnnotations.pop")
#  define _LIBCPP_BEGIN_UNVERSIONED_NAMESPACE_STD                                                                          _LIBCPP_PUSH_EXTENSION_DIAGNOSTICS _LIBCPP_END_EXPLICIT_ABI_ANNOTATIONS namespace _LIBCPP_NAMESPACE_VISIBILITY std {
#  define _LIBCPP_END_UNVERSIONED_NAMESPACE_STD } _LIBCPP_BEGIN_EXPLICIT_ABI_ANNOTATIONS _LIBCPP_POP_EXTENSION_DIAGNOSTICS
#  define _LIBCPP_BEGIN_NAMESPACE_STD _LIBCPP_BEGIN_UNVERSIONED_NAMESPACE_STD inline namespace _LIBCPP_ABI_NAMESPACE {
#  define _LIBCPP_END_NAMESPACE_STD } _LIBCPP_END_UNVERSIONED_NAMESPACE_STD
#define _LIBCPP_TOSTRING2(x) #x
#define _LIBCPP_TOSTRING(x) _LIBCPP_TOSTRING2(x)
#define _LIBCPP_CONCAT_IMPL(_X, _Y) _X##_Y
#define _LIBCPP_CONCAT(_X, _Y) _LIBCPP_CONCAT_IMPL(_X, _Y)
#  define _LIBCPP_VERSION 230102
#    define _NOEXCEPT noexcept
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_LIBC_LLVM_LIBC 0
#  define _LIBCPP_PREFERRED_OVERLOAD __attribute__((__enable_if__(true, "")))
#    define _LIBCPP_VISIBILITY(vis) __attribute__((__visibility__(vis)))
#  define _LIBCPP_HIDDEN _LIBCPP_VISIBILITY("hidden")
#    define _LIBCPP_NAMESPACE_VISIBILITY __attribute__((__type_visibility__("default")))
#  define _LIBCPP_EXCLUDE_FROM_EXPLICIT_INSTANTIATION __attribute__((__exclude_from_explicit_instantiation__))
#  define _LIBCPP_HARDENING_SIG n // "none"
#  define _LIBCPP_ASSERTION_SEMANTIC_SIG q
#  define _LIBCPP_EXCEPTIONS_SIG n
#define _LIBCPP_ODR_SIGNATURE                                                                                            _LIBCPP_CONCAT(                                                                                                            _LIBCPP_CONCAT(_LIBCPP_CONCAT(_LIBCPP_HARDENING_SIG, _LIBCPP_ASSERTION_SEMANTIC_SIG), _LIBCPP_EXCEPTIONS_SIG),         _LIBCPP_VERSION)
#  define _LIBCPP_HIDE_FROM_ABI                                                                                            _LIBCPP_HIDDEN _LIBCPP_EXCLUDE_FROM_EXPLICIT_INSTANTIATION                                                             __attribute__((__abi_tag__(_LIBCPP_TOSTRING(_LIBCPP_ODR_SIGNATURE))))
#  define _LIBCPP_USING_IF_EXISTS __attribute__((__using_if_exists__))
#    define _LIBCPP_STD_VER 23
#  define _LIBCPP_PUSH_EXTENSION_DIAGNOSTICS
#  define _LIBCPP_POP_EXTENSION_DIAGNOSTICS
#  define _LIBCPP_PUSH_ABI_PRAGMA_DIAGNOSTICS
#  define _LIBCPP_POP_ABI_PRAGMA_DIAGNOSTICS
#define _LIBCPP_END_EXPLICIT_ABI_ANNOTATIONS                                                                             _LIBCPP_PUSH_ABI_PRAGMA_DIAGNOSTICS                                                                                    _Pragma(_LIBCPP_TOSTRING(clang attribute _LibcxxExplicitABIAnnotations.push(                                               __attribute__((__exclude_from_explicit_instantiation__,                                                                               __visibility__("hidden"),                                                                                              __abi_tag__(_LIBCPP_TOSTRING(_LIBCPP_ODR_SIGNATURE)))),                                                 apply_to = function))) _LIBCPP_POP_ABI_PRAGMA_DIAGNOSTICS
#define _LIBCPP_BEGIN_EXPLICIT_ABI_ANNOTATIONS _Pragma("clang attribute _LibcxxExplicitABIAnnotations.pop")
#  define _LIBCPP_BEGIN_UNVERSIONED_NAMESPACE_STD                                                                          _LIBCPP_PUSH_EXTENSION_DIAGNOSTICS _LIBCPP_END_EXPLICIT_ABI_ANNOTATIONS namespace _LIBCPP_NAMESPACE_VISIBILITY std {
#  define _LIBCPP_END_UNVERSIONED_NAMESPACE_STD } _LIBCPP_BEGIN_EXPLICIT_ABI_ANNOTATIONS _LIBCPP_POP_EXTENSION_DIAGNOSTICS
#  define _LIBCPP_BEGIN_NAMESPACE_STD _LIBCPP_BEGIN_UNVERSIONED_NAMESPACE_STD inline namespace _LIBCPP_ABI_NAMESPACE {
#  define _LIBCPP_END_NAMESPACE_STD } _LIBCPP_END_UNVERSIONED_NAMESPACE_STD
#define _LIBCPP_TOSTRING2(x) #x
#define _LIBCPP_TOSTRING(x) _LIBCPP_TOSTRING2(x)
#define _LIBCPP_CONCAT_IMPL(_X, _Y) _X##_Y
#define _LIBCPP_CONCAT(_X, _Y) _LIBCPP_CONCAT_IMPL(_X, _Y)
#   define __cpp_lib_char8_t                            201907L
# define __cpp_lib_constexpr_string                     201907L
#   define __cpp_lib_format                             202110L
# define __cpp_lib_string_view                          201803L
# define __cpp_lib_ranges                               202406L
