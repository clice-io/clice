load("@rules_cc//cc:cc_library.bzl", "cc_library")

# The header-only C++ runtime: the registry's flatbuffers module drags in grpc,
# rules_go and the JS rules, at versions Bazel 9 rejects.
cc_library(
    name = "flatbuffers",
    hdrs = glob(["include/flatbuffers/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
