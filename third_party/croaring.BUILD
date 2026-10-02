load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "croaring",
    srcs = glob(["src/**/*.c"], allow_empty = True),
    hdrs = glob(["include/**/*.h", "cpp/**/*.hh"], allow_empty = True),
    copts = ["-w"],
    includes = ["include", "cpp"],
    visibility = ["//visibility:public"],
)
