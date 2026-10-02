load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "lmdb",
    srcs = ["mdb.c", "midl.c", "midl.h"],
    hdrs = ["lmdb.h"],
    copts = ["-w"],
    includes = ["."],
    linkopts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-lpthread"],
    }),
    visibility = ["//visibility:public"],
)
