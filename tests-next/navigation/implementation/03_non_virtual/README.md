---
issues: [clangd#854]
---

# Non-virtual function

Non-virtual declarations do not navigate to out-of-line definitions yet

Go-to-implementation on a non-virtual function declaration does not reach
its out-of-line definition and returns nothing.
