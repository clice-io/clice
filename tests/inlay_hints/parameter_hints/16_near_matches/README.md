---
issues: [clangd#2248]
option: inlay_hints.prefixed_names
---

# Prefixed parameter names

An argument named like its parameter without a one-letter prefix needs no hint

`param` passed to `aParam` and `buffer` passed to `pBuffer` count as the same name. `inlay_hints.prefixed_names` shows these hints again.
