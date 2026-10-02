load("@@//third_party:libclang.bzl", "native_archives")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

# zlib and zstd come from the registry, as libdwarf needs them too.
filegroup(
    name = "bitcode_archives",
    srcs = glob(["lib/*.a"], exclude = ["lib/libz.a", "lib/libzstd.a"]),
)

native_archives(
    name = "native_archives",
    srcs = [":bitcode_archives"],
)

config_setting(
    name = "bitcode",
    define_values = {"llvm": "bitcode"},
)

# LLVM's link interface beyond its own archives is only recorded in its CMake
# package (LLVMExports.cmake); here it is kept by hand, per platform.
cc_library(
    name = "llvm",
    srcs = select({
        ":bitcode": [":bitcode_archives"],
        "//conditions:default": [":native_archives"],
    }),
    hdrs = glob(["include/**"]),
    defines = ["CLANG_BUILD_STATIC=1"],
    includes = ["include"],
    linkopts = select({
        "@platforms//os:macos": ["-lxcselect", "-lpthread"],
        "@platforms//os:windows": ["-lversion", "-lntdll", "-lole32", "-loleaut32", "-luuid", "-lws2_32", "-lpsapi", "-lshell32", "-ladvapi32"],
        "//conditions:default": ["-lpthread", "-ldl"],
    }),
    visibility = ["//visibility:public"],
    deps = ["@zlib", "@zstd"],
)

# The resource directory clice looks for next to its executable.
filegroup(
    name = "resource_dir",
    srcs = glob(["lib/clang/**"]),
    visibility = ["//visibility:public"],
)
