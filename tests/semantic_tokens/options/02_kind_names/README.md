---
option: semantic_tokens.kinds
---

# Kind names

`kinds` renames the token kinds the server announces

Two kinds given one name merge into it, and a kind renamed to `""` is not
sent, leaving the editor's own color. The names are announced once, when the
editor connects: a change takes a restart.
