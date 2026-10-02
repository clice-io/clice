load("@rules_cc//cc:cc_library.bzl", "cc_library")

# As kotatsu configures it (src/zest/CMakeLists.txt) and cpptrace's
# Autoconfig.cmake picks for each platform. The registry's module builds -O0
# with libunwind (nongnu), which clashes with the LLVM libunwind xclang links.
cc_library(
    name = "src_headers",
    hdrs = glob(["src/**/*.hpp", "src/**/*.h"], allow_empty = True),
    strip_include_prefix = "src",
)

cc_library(
    name = "cpptrace",
    srcs = glob(["src/**/*.cpp"]),
    hdrs = glob(["include/cpptrace/**/*.hpp", "include/ctrace/*.h"]),
    defines = ["CPPTRACE_STATIC_DEFINE"],
    includes = ["include"],
    linkopts = select({
        "@platforms//os:windows": ["-ldbghelp"],
        "@platforms//os:linux": ["-ldl"],
        "//conditions:default": [],
    }),
    local_defines = [
        "HAS_ATTRIBUTE_PACKED",
        "CPPTRACE_HAS_CXX_EXCEPTION_TYPE",
        "CPPTRACE_DEMANGLE_WITH_CXXABI",
    ] + select({
        "@platforms//os:windows": [
            "CPPTRACE_GET_SYMBOLS_WITH_DBGHELP",
            "CPPTRACE_UNWIND_WITH_DBGHELP",
            "NOMINMAX",
        ],
        "@platforms//os:macos": [
            "CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF",
            "CPPTRACE_UNWIND_WITH_EXECINFO",
            "CPPTRACE_HAS_MACH_VM",
        ],
        "//conditions:default": [
            "CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF",
            "CPPTRACE_UNWIND_WITH_UNWIND",
            "CPPTRACE_HAS_DLADDR1",
        ],
    }),
    visibility = ["//visibility:public"],
    deps = [":src_headers"] + select({
        "@platforms//os:windows": [],
        "//conditions:default": ["@libdwarf//:dwarf"],
    }),
)
