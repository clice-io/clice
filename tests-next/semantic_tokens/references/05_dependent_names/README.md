---
issues: [clangd#154, clangd#297]
---

# Dependent names

Dependent names resolve through known primary templates

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration and currently get no token;
heuristic coloring for such names remains open.
