---
issues: [clice#1111]
---

# Class templates

A class template's operations are declared `= default`, and each instantiation deletes the ones it cannot perform, as the compiler does

`Box<int>` still copies and `Box<MoveOnly>` still only moves. Declaring a move deletes the implicit copies in every instantiation, so a template that declares its move constructor gets `= delete` copy declarations.
