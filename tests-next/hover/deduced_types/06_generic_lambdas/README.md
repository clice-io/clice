---
issues: [clangd#493, clangd#1768]
---

# Generic lambda parameters

A generic lambda's `auto` shows the type a call deduces, but its parameter and the parameter's members do not

Inside the body the parameter reads as `auto`, and `point.x` has no card.
