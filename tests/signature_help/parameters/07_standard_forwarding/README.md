---
issues: [clangd#638]
---

# Standard forwarding functions

`make_unique`, `make_shared` and the containers' `emplace` functions show the constructors of what they build

Each constructor is listed with the parameters the call passes its arguments
to, after the function's own ones such as the key of a map's `try_emplace`.
An aggregate lists its fields.
