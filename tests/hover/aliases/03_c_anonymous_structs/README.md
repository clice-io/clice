---
issues: [clangd#2219]
---

# C anonymous struct typedefs

In C, a struct named only by its typedef shows under the typedef's name

The definition line still spells it `struct Point`, a name C does not
know for it.
