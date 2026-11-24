---
issues: [clice#1152]
---

# Initializer lists

A delegating or base initializer's name leads to the constructor it calls

Go-to-definition, declaration, hover, highlights and references on the
class name of `Widget() : Widget(42)` or `: Base(1)` answer the constructor
the initializer calls; go-to-type-definition answers the class. Its
parenthesis still leads to the constructor, without counting as a second
reference.
