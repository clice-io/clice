---
option: diagnostics.clang_tidy_slow_checks
---

# Running the slow checks

`clang_tidy_slow_checks = true` runs every check the `.clang-tidy` enables

Checks too slow for an editor, such as `misc-const-correctness`, run on open files only when asked for: every compile of the file waits for them.
