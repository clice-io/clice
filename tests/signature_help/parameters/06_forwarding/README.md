---
issues: [clangd#517, clangd#638]
---

# Forwarding functions

A call that forwards its arguments to a constructor shows the constructor's parameters

A function template whose definition builds a class from its trailing pack —
`new T(args...)`, `T(args...)`, `T{args...}`, a variable built so, or
`std::make_unique<T>(args...)` — shows the constructors of the class it
builds, each after the function's own parameters before the pack. A member
of that class reaches its private constructors too. A pack built into a type
made of the arguments' own types shows as written.
