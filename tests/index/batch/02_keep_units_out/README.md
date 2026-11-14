---
option: rules.index
---

# Keep units out of indexing

A rule with `index = false` keeps its units out; they still compile when opened

Every file a compilation database lists is a unit, a header too: one the
build lists (as some generators do) compiles and indexes on its own, beside
the sources including it. A rule over `**/*.h` with `index = false` keeps
those out.
