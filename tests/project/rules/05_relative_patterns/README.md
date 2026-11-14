---
option: rules.patterns
---

# Patterns start at the root

A relative pattern matches paths under the project root, where `clice.toml` sits

The rule appends `-DFEATURE` to the files under `src/`: `src/main.cpp` compiles, while `other.cpp`, beside `clice.toml`, misses the macro.
