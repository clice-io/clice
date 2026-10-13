---
issues: [clangd#638]
---

# Standard forwarding functions

`make_unique`, `make_shared` and the containers' `emplace` functions show the constructors of what they build

The constructors that can take the arguments written so far are listed, the
best match first, each with its parameters after the function's own ones,
such as the key of a map's `try_emplace`. An aggregate lists its fields.
