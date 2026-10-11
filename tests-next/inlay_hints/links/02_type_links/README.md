---
issues: [clangd#1535]
---

# Clickable type names

Each type name in a type hint links to its declaration: clicking it goes to the definition, hovering it shows the type's card

A class declared ahead of its definition links to that declaration, from which go-to-definition reaches the definition. Template arguments link one by one; builtin types and punctuation stay plain text.
