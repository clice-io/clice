---
issues: [clangd#212]
---

# Template specialization navigation

Go-to-definition on the name of an explicit specialization resolves to the
specialization itself; stepping from it to the primary template it
specializes is not offered
