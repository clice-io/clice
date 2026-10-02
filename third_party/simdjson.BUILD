load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "src_headers",
    textual_hdrs = glob(["src/**/*.h", "src/**/*.cpp"], exclude = ["src/simdjson.cpp"], allow_empty = True),
    strip_include_prefix = "src",
)

cc_library(
    name = "simdjson",
    srcs = ["src/simdjson.cpp"],
    hdrs = glob(["include/**/*.h"], allow_empty = True),
    defines = [
        "SIMDJSON_EXCEPTIONS=0",
        "SIMDJSON_THREADS_ENABLED=1",
    ],
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [":src_headers"],
)
