"""How the third-party library modules of modules/ are built and used."""

# What every module unit compiles with: a module loads only into a
# compilation that matches it in exceptions and RTTI, and the sandbox of an
# importer holds no module sources, so the BMI carries them.
MODULE_COPTS = [
    "-fno-rtti",
    "-fno-exceptions",
    "-Xclang",
    "-fmodules-embed-all-files",
]

# name -> (the modules it imports, the libraries whose headers it wraps), as
# `clice modularize` printed them; which target provides an include root is
# the build's knowledge.
WRAPPERS = {
    "llvm": ([], ["@libclang//:headers"]),
    "clang": (["llvm"], ["@libclang//:headers"]),
    "simdjson": ([], ["@simdjson"]),
    "kota": (["simdjson"], [
        "@croaring",
        "@kotatsu//:codec_flatbuffers",
        "@kotatsu//:codec_json",
        "@kotatsu//:codec_toml",
        "@kotatsu//:deco",
        "@kotatsu//:ipc_json",
        "@kotatsu//:ipc_lsp",
        "@kotatsu//:option",
        "@kotatsu//:support",
        "@kotatsu//:zest_async",
    ]),
    "lmdb": ([], ["@lmdb"]),
    "spdlog": ([], ["@spdlog"]),
}

def mirror_include(library):
    """The include flag putting a library's emptied headers first."""
    return "-Imodules/mirror/" + library

# What a target compiling clice's module units adds, with a dependency on
# //modules and the cpp_modules feature: the wrapped headers emptied for the
# headers that stay headers its global module fragments include. A reduced
# BMI drops the global module fragment's declarations the purview never
# names.
PROGRAM_COPTS = MODULE_COPTS + ["-fno-modules-reduced-bmi"] + [
    mirror_include(library)
    for library in ["std"] + list(WRAPPERS)
]
