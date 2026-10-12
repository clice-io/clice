---
issues: [clangd#1931]
---

# Implicit conversion operators

A condition or `!` navigates to the conversion operator it runs

Go-to-definition on the parenthesis of a condition, or on the `!`, `&&`,
`||` or `?` whose operand converts, reaches the user-defined conversion
operator that tests the operand.
