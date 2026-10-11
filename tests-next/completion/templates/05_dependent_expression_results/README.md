---
issues: [clangd#443]
---

# Dependent expression results

A member access on what a subscript or a member call returns inside a template completes the members of the class it evaluates to

`rows[0].` on a `Vec<Vec<T>>` lists the members of `Vec<T>`, followed through the container's `reference` alias the way the standard containers declare it. Where a member has a `const` overload, the constness of the object picks the one called, a data member reached through a const object counting as const, and `->` on a returned iterator reaches the element.
