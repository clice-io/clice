---
issues: [clangd#1363]
---

# Inherited constructors

Constructors a class inherits are missing from its constructor list

Only the implicit copy and move constructors show, not the `Base(int, int)`
that `using Base::Base` brings in and the call actually uses.
