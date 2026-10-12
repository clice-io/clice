---
option: rules.compile_commands
---

# Databases in priority order

A file several databases list takes its command from the first one, and the others stay on offer as contexts

Database `a` lists `main.cpp` with `-DFROM_A`; database `b` lists it with `-DFROM_B` and also lists `other.cpp`. `main.cpp` compiles with the command from `a` and can be switched to the one from `b`, while `other.cpp` takes its command from `b`.
