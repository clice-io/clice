---
issues: [clangd#1344]
---

# Overload disambiguation

Workspace symbol results omit parameter types, leaving overloads ambiguous

Querying an overloaded name finds every overload, but each entry
carries only the bare name — nothing tells the two `process` results
apart short of opening both locations.
