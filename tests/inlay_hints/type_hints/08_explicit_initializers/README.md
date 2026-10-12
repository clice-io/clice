---
issues: [clangd#1749]
---

# Explicitly spelled initializers

A type the initializer already spells, in a cast or a functional cast, gets no hint

A class template whose arguments the initializer leaves to deduction is still
hinted.
