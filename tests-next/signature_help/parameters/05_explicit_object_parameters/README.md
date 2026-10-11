---
issues: [clangd#2284]
---

# Explicit object parameters

A C++23 `this` parameter shows in the label and takes the first argument's mark

The object a member is called on is not an argument, yet the label lists
`this Shape &self` and marks it while the cursor is on the argument for `dx`.
