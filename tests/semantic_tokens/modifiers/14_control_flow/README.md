# Control flow

Keywords that transfer control and functions that never return are `controlFlow`

`if`, `for`, `return` and the other statements that branch, loop or leave
carry it, telling them apart from declaration keywords such as `int` or
`const`. A `[[noreturn]]` function carries it too, as does a macro whose
expansion can leave the function.
