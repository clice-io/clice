---
issues: [clangd#2242]
---

# Constructor calls through forwarding functions

Incoming calls of a constructor do not include the call sites that reach it
through a perfect-forwarding factory
