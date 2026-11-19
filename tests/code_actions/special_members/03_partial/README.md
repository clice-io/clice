---
issues: [clice#1111]
---

# Only the missing members

A class that already declares some operations gains gets the declarations it lacks

Notably, class declaring only its copy constructor and copy assignment gets two `= delete` move declarations; one declaring only the move operations gets two `= delete` copy declarations.
