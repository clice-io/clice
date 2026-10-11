---
issues: [clangd#807]
---

# Missing enum cases

A switch over an enum receives the enumerators it does not handle, followed by a `break`

Enumerators sharing a handled value are considered covered.
