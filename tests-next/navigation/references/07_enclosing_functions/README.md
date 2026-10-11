---
issues: [clangd#177]
---

# Reference enclosing context

Each reference is reported as a bare location; the name of the function that
encloses it is not attached, so results carry no context beyond the file and
line
