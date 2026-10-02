load("@rules_cc//cc:cc_library.bzl", "cc_library")

# LLVM's link interface beyond its own archives is only recorded in its CMake
# package (LLVMExports.cmake); here it is kept by hand, per platform.
cc_library(
    name = "llvm",
    srcs = glob(["lib/*.a"]),
    hdrs = glob(["include/**"]),
    defines = ["CLANG_BUILD_STATIC=1"],
    includes = ["include"],
    linkopts = select({
        "@platforms//os:macos": ["-lz", "-lxcselect", "-lpthread"],
        "@platforms//os:windows": ["-lversion", "-lntdll", "-lole32", "-loleaut32", "-luuid", "-lws2_32", "-lpsapi", "-lshell32", "-ladvapi32"],
        "//conditions:default": ["-lpthread", "-ldl"],
    }),
    visibility = ["//visibility:public"],
)

# The resource directory clice looks for next to its executable.
filegroup(
    name = "resource_dir",
    srcs = glob(["lib/clang/**"]),
    visibility = ["//visibility:public"],
)
