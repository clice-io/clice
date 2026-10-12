---
issues: [clangd#2055]
---

# `auto` behind pointers and references

Pointers, references and arrays around the deduced type are stripped:
`auto*`, `const auto&`, `auto&&`, `decltype(auto)` and an `auto` deduced
as a pointer all reach the class

The `auto` of a structured binding reaches the type of the object it
decomposes.
