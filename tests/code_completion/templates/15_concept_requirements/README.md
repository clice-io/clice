---
issues: [clangd#1103]
---

# Concept requirements

Inside a constrained template, the members the concept requires complete after a dot

A member the concept calls completes as a method with the types of the arguments it is called with; after `T::` the nested types it names complete too.
