---
issues: [clice#1111]
---

# Only the missing members

A class that already declares some operations gets the declarations it lacks, as the compiler has them

A class declaring its move operations gets two `= delete` copy declarations: declaring a move deletes the implicit copies. A class declaring its copy constructor gets its copy assignment and no move declaration: the compiler declares no move there, rvalues are copied by the user's constructor, and a declared move would change that.
