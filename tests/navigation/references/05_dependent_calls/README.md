---
issues: [clangd#258, clangd#675]
---

# Dependent template references

Find references on a member does not include dependent call sites in a
template, even when the template is instantiated with the member's class
