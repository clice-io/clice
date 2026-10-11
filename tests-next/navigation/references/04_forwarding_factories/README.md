---
issues: [clangd#716, clangd#1872]
---

# References through forwarding functions

Find references on a constructor does not include call sites that reach it
indirectly through a perfect-forwarding factory
