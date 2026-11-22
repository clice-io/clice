---
issues: [clice#1111]
---

# Impossible operations deleted

A copy operation a member makes impossible is declared `= delete`, and a move it makes impossible is left undeclared

A member class that only moves makes the copy operations impossible; a reference member or a `const` member makes the assignments impossible. The move assignment then stays undeclared: overload resolution never chooses a move the compiler deletes, but would choose one declared `= delete`.
