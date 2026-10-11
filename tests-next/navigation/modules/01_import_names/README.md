---
issues: [clangd#2310]
---

# Module import navigation

Go-to-definition on the name in an `import` declaration opens the module
interface unit that exports it, and uses of an imported symbol reach its
definition in that unit
