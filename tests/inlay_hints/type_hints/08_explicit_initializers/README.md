---
issues: [clangd#1749, clangd#2603]
option: inlay_hints.spelled_types
---

# Explicitly spelled initializers

A type the initializer already names gets no hint

A cast, a functional cast, a `new`, a factory naming the type as its template argument (`make_owner<Box<int>>()` for an `Owner<Box<int>>`) and a lambda all say what the variable holds. A class template whose arguments the initializer leaves to deduction shows them where it is written instead, and a factory whose result has arguments the call does not name still hints. `inlay_hints.spelled_types` shows every hint.
