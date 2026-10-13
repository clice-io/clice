---
issues: [clangd#2324]
---

# Forwarding resolution

Packs forwarded through wrappers resolve to the target's parameter names

A plain pass-through works without `std::forward`, a pack between fixed leading and trailing arguments resolves to the parameters it lands in, and a pack handed on to a callable object resolves to its call operator. A pack that ends up initializing a scalar has no parameter to name.
