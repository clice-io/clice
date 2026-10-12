---
issues: [clangd#914]
---

# Fuzzy matching

A query matches a name as a subsequence aligned to its words

`LinLis` finds `LinkedList` and `pconf` finds `parse_config`: after its
first letter, every letter of the query either continues a run or starts
a word of the name, so `pcfg` finds nothing — its `f` lands in the middle
of `config`.
