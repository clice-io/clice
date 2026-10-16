---
issues: [clangd#1661, clangd#2059]
---

# Preprocessor conditionals

Each branch of a conditional forms a folding range up to the directive that ends it, which stays visible

A branch that would cut across a block or a section, such as the
`#ifdef __cplusplus` around `extern "C" {` in a C header, gives way to it, so
the folds always nest.
