---
issues: [clice#1111]
---

# Members made explicit

A class whose members copy and move gains explicit `= default` declarations of its copy and move operations

The declarations are appended to the end of the class body: the default constructor when the class has no constructor, then the copy constructor, move constructor, copy assignment and move assignment. They declare what the compiler declared implicitly, so the uses of the class compile and behave as before; only an aggregate loses its aggregate initialization.
