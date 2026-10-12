---
issues: [clangd#493, clangd#1768]
---

# Generic lambda parameters

An `auto` parameter of a lambda gets no member completion inside its body yet

The lambda is called with a `Point`, directly or through an algorithm, but its body is still a template when the completion runs.
