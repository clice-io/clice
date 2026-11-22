---
issues: [clice#1111]
---

# Impossible operations deleted

An operation a member makes impossible is declared `= delete` instead of `= default`

A member class that only moves makes the copy operations impossible. A reference member or a `const` member makes the assignments impossible: the copy assignment is declared deleted, and the move assignment stays undeclared, since overload resolution never chooses the move the compiler deletes, but would choose one declared `= delete`.
