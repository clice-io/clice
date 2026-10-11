---
issues: [clangd#1298, clangd#1357]
option: inlay_hints.type_name_limit
---

# Type sugar length limits

Aliases keep their spelling, and an over-long type falls back to its sugared name

A type without a spelling short enough is dropped, and default template arguments never print. The budget is `inlay_hints.type_name_limit`; `0` lifts it.
