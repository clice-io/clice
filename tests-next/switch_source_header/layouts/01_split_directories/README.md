# Headers apart from sources

A header under `include/` finds its source under `src/`, ahead of another file of the same name that defines none of its declarations

Away from the header's own directory, a file of the same name counts
only when one of the two includes the other — `tools/shape.cpp` includes
nothing and is no counterpart — and of those only the nearest:
`src/legacy/shape.cpp` lies a directory deeper than `src/shape.cpp`.
