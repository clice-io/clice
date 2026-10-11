---
issues: [clangd#1081]
---

# Range-for references

Find references on `begin` reports only its own declaration; the range-based
for loop that implicitly calls it is not included among the references
