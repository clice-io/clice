load("@rules_cc//cc:cc_library.bzl", "cc_library")

# kotatsu includes <toml++/toml.hpp>; the registry's module exposes only the
# single-header toml.hpp.
cc_library(
    name = "tomlplusplus",
    hdrs = glob(["include/toml++/**/*.hpp", "include/toml++/**/*.inl", "include/toml++/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
