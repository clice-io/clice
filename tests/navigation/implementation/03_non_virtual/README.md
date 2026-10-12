---
issues: [clangd#854]
---

# Non-virtual function

A function nothing overrides is implemented by its definition

Go-to-implementation on the declaration of a function that is not
overridden, virtual or not, reaches its out-of-line definition.
