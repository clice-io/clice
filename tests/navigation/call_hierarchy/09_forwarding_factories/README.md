---
issues: [clangd#2242]
---

# Constructor calls through forwarding functions

Incoming calls of a constructor include the perfect-forwarding factory that
runs it, not the call sites that reach it through the factory
