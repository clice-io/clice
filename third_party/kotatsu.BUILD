load("@rules_cc//cc:cc_library.bzl", "cc_library")

# kotatsu as clice configures it (cmake/package.cmake): no exceptions, no RTTI,
# simdjson / flatbuffers / toml codecs, async + ipc, deco, zest.
_COPTS = ["-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra"]

cc_library(
    name = "headers",
    hdrs = glob(["include/**/*.h", "include/**/*.inl"], allow_empty = True),
    defines = [
        "KOTA_ENABLE_EXCEPTIONS=0",
        "KOTA_ENABLE_RTTI=0",
    ] + select({
        "@platforms//os:windows": ["_CRT_SECURE_NO_WARNINGS"],
        "//conditions:default": [],
    }),
    includes = ["include"],
)

alias(
    name = "meta",
    actual = ":headers",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "support",
    srcs = ["src/support/glob_pattern.cpp"],
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":headers"],
)

cc_library(
    name = "codec_json",
    visibility = ["//visibility:public"],
    deps = [":headers", "@simdjson"],
)

cc_library(
    name = "codec_flatbuffers",
    visibility = ["//visibility:public"],
    deps = [":headers", "@flatbuffers//:runtime_cc"],
)

cc_library(
    name = "codec_toml",
    defines = ["TOML_EXCEPTIONS=0"],
    visibility = ["//visibility:public"],
    deps = [":headers", "@tomlplusplus"],
)

cc_library(
    name = "option",
    srcs = glob(["src/deco/option/*.cc"], allow_empty = True),
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":headers"],
)

cc_library(
    name = "deco",
    srcs = glob(["src/deco/facade/*.cc"], allow_empty = True),
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":headers", ":option"],
)

cc_library(
    name = "async",
    srcs = glob(["src/async/**/*.cpp", "src/async/**/*.h"], allow_empty = True),
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":headers", "@libuv"],
)

cc_library(
    name = "ipc",
    srcs = [
        "src/ipc/codec/bincode.cpp",
        "src/ipc/codec/json.cpp",
        "src/ipc/recording_transport.cpp",
        "src/ipc/transport.cpp",
    ],
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":async", ":codec_json", ":headers"],
)

cc_library(
    name = "ipc_lsp",
    srcs = glob(["src/ipc/lsp/*.cpp"], allow_empty = True),
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":headers", ":ipc"],
)

cc_library(
    name = "zest",
    srcs = glob(["src/zest/*.cpp"], allow_empty = True),
    copts = _COPTS,
    visibility = ["//visibility:public"],
    deps = [":deco", ":headers", ":support", "@cpptrace"],
)
