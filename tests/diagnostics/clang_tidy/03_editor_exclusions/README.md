---
option: diagnostics.clang_tidy_slow_checks
---

# Checks left out

A check too slow for an editor is named, not dropped silently

Open files run neither the checks too slow for an editor nor those unreliable on code being edited. clice names the slow ones the `.clang-tidy` enables once, with how to run them; `clice lint` runs both.
