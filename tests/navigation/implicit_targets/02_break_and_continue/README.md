---
issues: [clangd#1921]
---

# `break` / `continue`

`break` and `continue` do not navigate to their enclosing control statement
yet

Go-to-definition on `break` or `continue` does not reach the head of the
loop or switch it controls.
