---
issues: [clangd#2275]
---

# Instantiated templates

A template body shows its hints once, but a variable whose type depends on the template shows none even when the only instantiation fixes it

With two instantiations deducing different types there is no one type to show.
