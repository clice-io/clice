---
issues: [clice#1111]
---

# Only the missing members

A class that already declares some operations gains only the declarations it lacks

A class declaring only its copy constructor and copy assignment gains the two move declarations, both `= delete`; one declaring only the move operations gains the two copy declarations, both `= delete`.
