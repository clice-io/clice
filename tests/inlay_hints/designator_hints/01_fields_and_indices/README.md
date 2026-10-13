---
issues: [clangd#1277]
option: inlay_hints.array_indices
---

# Field and index designators

Positional aggregate initialization shows `.field=`, and `[index]=` once enabled

Array indices are off by default, `inlay_hints.array_indices` turns them on: a table would carry one on every element. Without them a path through an array shows its fields alone; with indices but no field designators it shows nothing. Array designators survive members whose size depends on a template parameter, and reserved member names are skipped rather than printed.
