---
issues: [clangd#1123]
option: inlay_hints.mutable_references
---

# Mutable reference markers

`&` flags arguments passed by non-const lvalue reference

The marker has a switch of its own, `inlay_hints.mutable_references`: with parameter names turned off it stays, in front of an empty name.
