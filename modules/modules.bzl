"""How the third-party libraries' module of modules/ is built and used."""

# What every module unit compiles with: a module loads only into a
# compilation that matches it in exceptions and RTTI, and a reduced BMI
# drops the global module fragment's declarations the purview never names,
# partial specializations among them.
MODULE_COPTS = [
    "-fno-rtti",
    "-fno-exceptions",
    "-fno-modules-reduced-bmi",
]

# The libraries whose headers deps.cppm wraps, as `clice modularize` found
# them; which target provides an include root is the build's knowledge.
LIBRARIES = [
    "@croaring",
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
