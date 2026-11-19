---
issues: [clice#1111]
---

# Impossible operations deleted

An operation a member makes impossible is declared `= delete` instead of `= default`

For example, a reference member or a `const` member makes the assignment operators impossible; a member class that only moves makes the copy operations impossible.
