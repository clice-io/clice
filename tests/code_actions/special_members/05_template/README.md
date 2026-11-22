---
issues: [clice#1111]
---

# Class templates

A class template's operations follow the language's rules, `= default` where the compiler would declare them, and each instantiation deletes those it cannot perform

`Box<int>` still copies and `Box<MoveOnly>` still only moves. Declaring a move deletes the implicit copies in every instantiation, so a template that declares its move constructor gets `= delete` copy declarations.
