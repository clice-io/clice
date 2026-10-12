---
issues: [clangd#2275]
---

# Instantiated templates

A template body shows its hints once, and a variable whose type depends on the template shows the type the template's only instantiation gives it

With two instantiations deducing different types there is no one type to show.
