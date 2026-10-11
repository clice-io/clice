---
issues: [clangd#550]
---

# Qualified name search

A qualified query names the containers the symbol must lie in

`net::Socket` finds `deep::net::Socket`: the qualifiers must appear in the
symbol's container chain in that order, with other containers allowed
around them, while a leading `::` demands exactly that chain. Replies to
a qualified query spell the qualified name, so editors that filter
results against the query text keep them.
