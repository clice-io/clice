# Empty and control-flow macros

A macro that expands to nothing is `empty`, one that can leave the function `controlFlow`

Both modifiers mark the macro's name at its definition and at every
expansion. The arguments an empty macro drops never reach the compiler
and are `inactive`, like the code of an untaken branch.
