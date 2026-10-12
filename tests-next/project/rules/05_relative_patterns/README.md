---
option: rules.patterns
---

# Patterns start at `clice.toml`

A relative pattern matches paths under the directory of the `clice.toml` that declares it

The rule appends `-DFEATURE` to the files under `src/`: `src/main.cpp` compiles, while `other.cpp`, beside `clice.toml`, misses the macro.
