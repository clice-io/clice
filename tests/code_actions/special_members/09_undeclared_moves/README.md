---
issues: [clice#1111]
---

# Rvalues keep copying

A move the compiler does not declare, or declares deleted, is left undeclared, and rvalues keep copying

A declared destructor or copy operation suppresses the implicit moves; a member that cannot move deletes them, and overload resolution then passes over them. Either way an rvalue is copied today. `= delete` would make it fail to compile, and `= default` on a class with a destructor would move members the user's code copied.
