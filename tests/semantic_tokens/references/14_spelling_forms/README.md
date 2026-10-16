---
issues: [clangd#1390, clangd#1627, clangd#2235, clangd#2547, clangd#484]
---

# Spelling forms

Names keep their kinds in the less common ways C++ spells them

An `auto` parameter is a keyword, not a template parameter; `new` behind
`::` an operator; the class of a pointer to member a class; the left
operand of an overloaded `=` is modified like a built-in one's; a member
template named after `template` is `unknown` until an instantiation.
