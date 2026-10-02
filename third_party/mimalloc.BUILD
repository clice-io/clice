load("@rules_cc//cc:cc_library.bzl", "cc_library")

# The static build (src/static.c includes every other source), configured as
# cmake/package.cmake does: no override, skip collecting on exit. The
# registry's module wants an MSVC import library on Windows.
cc_library(
    name = "mimalloc",
    srcs = ["src/static.c"],
    hdrs = glob(["include/*.h"]),
    includes = ["include"],
    linkopts = select({
        "@platforms//os:windows": ["-lpsapi", "-lshell32", "-luser32", "-ladvapi32", "-lbcrypt"],
        "//conditions:default": ["-pthread"],
    }),
    local_defines = [
        "MI_STATIC_LIB",
        "MI_SKIP_COLLECT_ON_EXIT=1",
    ],
    textual_hdrs = glob(["src/**/*.c", "src/**/*.h", "include/mimalloc/*.h"], exclude = ["src/static.c"]),
    visibility = ["//visibility:public"],
)
