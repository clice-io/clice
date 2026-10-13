---
issues: [clangd#2275]
---

# Instantiated templates

An `auto` variable inside a class template shows the type the template's only instantiation gives it

With two instantiations deducing different types it shows the type as the template writes it.
