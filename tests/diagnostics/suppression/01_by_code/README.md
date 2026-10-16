---
option: diagnostics.suppress
---

# Suppressing diagnostics

`suppress` hides diagnostics by their code

A diagnostic clang gets wrong on some code can be hidden by the code it shows with: a compiler warning's option, clang's name for any other diagnostic, or a clang-tidy check. The file's other features work as before, and `clice lint` still reports it.
