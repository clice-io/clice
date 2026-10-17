---
issues: [clangd#346]
---

# Nested macro references

Find references on a macro includes its mentions in the bodies of other
macros that expand somewhere

A body that never expands names nothing: what its names mean is up to each
expansion.
