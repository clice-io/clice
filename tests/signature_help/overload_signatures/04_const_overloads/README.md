---
issues: [clangd#50]
---

# Const overloads

A non-const object lists both the const and the non-const overload, and the two read the same

The label leaves out the trailing `const`, so nothing tells them apart.
