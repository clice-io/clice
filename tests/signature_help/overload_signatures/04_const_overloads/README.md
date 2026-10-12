---
issues: [clangd#50]
---

# Const overloads

Of a const and a non-const overload taking the same parameters, only the one the object calls shows

A non-const object calls the non-const overload; a const object can call only
the const one, whose label ends in `const`.
