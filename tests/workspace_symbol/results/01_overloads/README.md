---
issues: [clangd#1344]
---

# Overload disambiguation

Workspace symbol results show each function's parameters, telling overloads apart

Querying an overloaded name finds every overload, each named with its
parameters: the two `process` results differ at a glance.
