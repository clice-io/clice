---
issues: [clangd#445]
---

# Define a declared method

A method declaration offers an inline body and an out-of-line definition after its class

The out-of-line definition repeats the declaration with `S::` before the name and drops what belongs to the declaration alone, such as default arguments.
