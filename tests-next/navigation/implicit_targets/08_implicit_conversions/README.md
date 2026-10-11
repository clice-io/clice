---
issues: [clangd#1931]
---

# Implicit conversion operators

Conversion contexts do not navigate to the invoked conversion operator yet

Go-to-definition from a context that runs a user-defined conversion, a
condition or `!`, does not reach the conversion operator.
