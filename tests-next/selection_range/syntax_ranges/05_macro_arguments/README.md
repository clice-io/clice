---
issues: [clangd#1324]
---

# Macro arguments

A name or expression written in a macro argument is selected where it is written, then the argument list and the whole invocation

Code a macro's replacement spells around the argument has no place in
the file of its own: the steps go from the argument straight to the
invocation.
