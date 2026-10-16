---
issues: [clangd#154, clangd#297]
---

# Dependent names

Dependent names resolve through known primary templates

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration: they are `unknown`, or a type
after `typename`. Every dependent name carries `dependent`.
