---
issues: [clangd#2487]
option: inlay_hints.endif
---

# Conditions after `#endif`

An `#endif` closing a tall conditional shows the condition it closes

`#ifdef NAME` shows `NAME`, `#ifndef NAME` shows `!NAME`, and `#if` its condition as written, in branches the build skips too; an `#endif` already followed by a comment gets none. Clicking the condition goes to its `#if`. On by default.
