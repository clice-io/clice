"""What the root BUILD.bazel builds with: the flags of clice's own sources
and programs, the release packages of its clice, and the benchmarks."""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files", "strip_prefix")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("@rules_pkg//pkg:zip.bzl", "pkg_zip")
load("@xclang//bazel:debug_symbols.bzl", "xclang_debug_symbols")
load("//modules:modules.bzl", "MODULE_COPTS")

# What clice's own sources compile with; the libraries keep their own flags.
COPTS = [
    "-Isrc",
    "-fno-rtti",
    "-fno-exceptions",
    "-Werror=return-type",
    "-Wno-deprecated-declarations",
    "-Wno-undefined-inline",
]

# What clice's module units compile with, with a dependency on //modules and
# the cpp_modules feature.
CLICE_COPTS = COPTS + MODULE_COPTS

# Link optimizations in optimized builds: what the test suites run is what
# releases ship. icf=safe (not =all) so address-taken functions keep C++
# pointer identity.
LINKOPTS = select({
    "//bazel:opt_macos": ["-Wl,--icf=safe"],
    "//bazel:opt": [
        "-Wl,--icf=safe",
        "-Wl,-O2",
    ],
    "//conditions:default": [],
})

def release_packages():
    """//:package and //:symbols, of the package's :bin/clice.

    The release archive is clice/{bin/clice, lib/clang, clice.toml, LICENSE},
    the symbol package clice.gsym for scripts/symbolize.py: the programs of
    these builds are the ones the test suites run.
    """
    pkg_files(
        name = "package_bin",
        srcs = [":bin/clice.stripped"],
        attributes = pkg_attributes(mode = "0755"),
        prefix = "clice/bin",
        renames = select({
            "@platforms//os:windows": {":bin/clice.stripped": "clice.exe"},
            "//conditions:default": {":bin/clice.stripped": "clice"},
        }),
    )

    pkg_files(
        name = "package_resource_dir",
        srcs = [":resource_dir"],
        prefix = "clice",
        strip_prefix = strip_prefix.from_pkg(),
    )

    pkg_files(
        name = "package_docs",
        srcs = [
            "LICENSE",
            "docs/clice.toml",
        ],
        prefix = "clice",
    )

    package = [
        ":package_bin",
        ":package_docs",
        ":package_resource_dir",
    ]

    pkg_tar(
        name = "package_tar",
        srcs = package,
        extension = "tar.gz",
        package_file_name = "clice.tar.gz",
        tags = ["manual"],
    )

    pkg_zip(
        name = "package_zip",
        srcs = package,
        package_file_name = "clice.zip",
        tags = ["manual"],
    )

    native.alias(
        name = "package",
        actual = select({
            "@platforms//os:windows": ":package_zip",
            "//conditions:default": ":package_tar",
        }),
    )

    # The GSYM, and the dSYM it comes from on macOS. --merged-functions keeps
    # the names of the functions ICF folded; one thread makes the file the
    # same bytes every time.
    xclang_debug_symbols(
        name = "debug_symbols",
        binary = ":bin/clice",
        gsymutil_args = [
            "--merged-functions",
            "--num-threads=1",
        ],
        tags = ["manual"],
    )

    native.filegroup(
        name = "gsym",
        srcs = [":debug_symbols"],
        output_group = "gsym",
        tags = ["manual"],
    )

    pkg_tar(
        name = "symbols_tar",
        srcs = [":gsym"],
        extension = "tar.xz",
        package_file_name = "clice-symbol.tar.xz",
        tags = ["manual"],
    )

    pkg_zip(
        name = "symbols_zip",
        srcs = [":gsym"],
        package_file_name = "clice-symbol.zip",
        tags = ["manual"],
    )

    native.alias(
        name = "symbols",
        actual = select({
            "@platforms//os:windows": ":symbols_zip",
            "//conditions:default": ":symbols_tar",
        }),
    )

def benchmark_programs(benchmarks):
    """bin/<benchmark> of benchmarks/<benchmark>.cpp each, and //:benchmarks."""
    for benchmark in benchmarks:
        cc_binary(
            name = "bin/" + benchmark,
            srcs = [
                "benchmarks/%s.cpp" % benchmark,
                "benchmarks/stats.h",
            ],
            copts = CLICE_COPTS,
            data = [":resource_dir"],
            features = ["cpp_modules"],
            linkopts = LINKOPTS,
            tags = ["manual"],
            deps = [
                ":server",
                "//bazel:llvm",
            ],
        )

    native.filegroup(
        name = "benchmarks",
        srcs = ["bin/" + benchmark for benchmark in benchmarks],
        tags = ["manual"],
    )
