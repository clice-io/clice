---
issues: [clangd#2139]
---

# Read/write classification of references

The reference reply carries only locations, so a reader cannot tell a write
from a read; annotating each result with its access kind is not offered
