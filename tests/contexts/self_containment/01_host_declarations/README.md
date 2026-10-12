# Headers that need their includer

A header that uses what its includer declares first still compiles, in the includer's context

`ruler.h` uses a type and a macro that `main.cpp` declares before including it, so the header does not compile on its own. clice finds that out and compiles it after what `main.cpp` declares, so it has no errors and hover works inside it.
