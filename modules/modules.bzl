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

# What a target compiling clice's sources adds, with a dependency on
# //modules and the cpp_modules feature: the wrapped headers emptied, the
# modules imported and their macros replayed.
PROGRAM_COPTS = ["-Imodules/mirror/" + mirror for mirror in ["std"] + list(WRAPPERS)] + [
    "-include",
    "modules/prelude.h",
]
