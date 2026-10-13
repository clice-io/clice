---
issues: [clangd#2331]
option: inlay_hints.deduced_template_arguments
---

# Deduced class template arguments

A class template whose arguments its initialization deduces shows them

The arguments follow the template's name wherever it is written without them: a declaration, a functional cast, a `new`. A variable initialized by such a cast shows no type of its own, the arguments already say it.
