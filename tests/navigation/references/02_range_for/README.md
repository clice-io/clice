---
issues: [clangd#1081]
---

# Range-for references

Find references on `begin` includes the range-based for loops that call it

A range-based for calls the range's `begin()` and `end()` without writing
their names; its `:` stands for those calls among the references.
