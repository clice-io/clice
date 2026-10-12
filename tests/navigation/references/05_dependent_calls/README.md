---
issues: [clangd#258, clangd#675]
---

# Dependent template references

Find references on a member includes its dependent call sites in an open file's template instantiated only with the member's class

Files that are not open contribute no such call sites.
