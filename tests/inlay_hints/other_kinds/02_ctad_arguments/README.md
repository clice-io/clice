---
issues: [clangd#2331]
option: inlay_hints.deduced_template_arguments
---

# Deduced class template arguments

A class template whose arguments its initialization deduces shows them

The arguments follow the template's name wherever it is written without them: a declaration, a functional cast, a `new`; they print as the template's own name takes them, whatever scope encloses it and whatever aliases or defaults a deduction guide wrote. A variable initialized by such a cast shows no type of its own while the arguments say it.
