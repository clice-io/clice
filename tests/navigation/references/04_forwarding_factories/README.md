---
issues: [clangd#716]
---

# References through forwarding functions

Find references on a constructor includes the construction inside a
perfect-forwarding factory, not the call sites that reach it through the
factory
