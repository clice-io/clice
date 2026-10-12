---
issues: [clangd#2620]
---

# Macros at call sites

Arguments spelled as macros hint; calls generated inside macro bodies do not

An object-like macro is one written argument. A macro covering several arguments has no place to anchor a hint, and code written as a macro argument keeps its hints.
