# Folding Ranges

Implementation: `src/feature/folding_ranges.cpp`

## Fold Kinds

- [x] Block folding (`{...}` — functions, classes, namespaces)
- [ ] Comment folding (multi-line `/* */` and consecutive `//`)
- [ ] Include region folding (consecutive `#include` blocks)
- [x] Preprocessor region folding (`#region`/`#endregion`, `#if`/`#endif`)

## Encoding

- [x] UTF-16 position encoding
- [x] Line-based folding ranges (start/end lines)
