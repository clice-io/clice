---
issues: [clangd#1877]
---

# Hint suppression

Named arguments and `/*name=*/` comments suppress parameter hints

An argument spelled like its parameter, or a bare member of that name, needs no hint; a qualified name or a member reached through an object is no match. A comment naming another parameter suppresses nothing.
