"""How the third-party libraries' module of modules/ is built and used."""

# What every module unit compiles with: a module loads only into a
# compilation that matches it in exceptions and RTTI, and the sandbox of an
# importer holds no module sources, so the BMI carries them.
MODULE_COPTS = [
    "-fno-rtti",
    "-fno-exceptions",
    "-Xclang",
    "-fmodules-embed-all-files",
]

# The libraries whose headers deps.cppm wraps, as `clice modularize` found
# them; which target provides an include root is the build's knowledge.
LIBRARIES = [
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
    "@libclang//:headers",
    "@lmdb",
    "@simdjson",
    "@spdlog",
]

# What a target compiling clice's module units adds, with a dependency on
# //modules and the cpp_modules feature. A reduced BMI drops the global
# module fragment's declarations the purview never names.
PROGRAM_COPTS = MODULE_COPTS + ["-fno-modules-reduced-bmi"]
