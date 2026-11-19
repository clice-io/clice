---
issues: [clice#1111]
---

# Members made explicit

A class whose members copy and move gains explicit `= default` declarations of all four operations

The declarations are appended to the end of the class body, in the order of copy constructor, move constructor, copy assignment and move assignment.
