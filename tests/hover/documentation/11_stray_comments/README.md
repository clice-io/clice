---
issues: [clangd#2148]
---

# Stray comments

A plain comment a blank line separates from a declaration is not its documentation

A `///` or `/** */` comment documents the declaration below it even across
a blank line, as Doxygen reads it.
