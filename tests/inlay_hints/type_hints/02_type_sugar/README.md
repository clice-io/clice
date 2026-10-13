---
issues: [clangd#1298, clangd#1357, clangd#2268]
option: inlay_hints.max_length.types
---

# Type sugar length limits

Aliases keep their spelling, and an over-long type keeps what of it fits

A type too long for the limit falls back to its sugared name, then writes the template arguments that do not fit as `…`, keeping its outermost name; a type whose outermost name does not fit shows nothing. Default template arguments never print. The limit is `inlay_hints.max_length.types`; `0` lifts it.
