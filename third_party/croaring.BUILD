load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "croaring",
    srcs = glob(["src/**/*.c"]),
    hdrs = glob(["include/**/*.h", "cpp/**/*.hh"]),
    copts = ["-w"],
    includes = ["include", "cpp"],
    visibility = ["//visibility:public"],
)
