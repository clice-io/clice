# Inlay Hints

Implementation: `src/feature/inlay_hints.cpp`

## Hint Kinds

### Parameter Name Hints
- [x] Named parameter hints at call sites
- [x] Skip when argument name matches parameter name
- [x] Skip for single-parameter calls with obvious semantics
- [x] Expand parameter packs (`underlying_pack_type` detection)
- [x] Resolve parameter from definition (not just declaration)

### Type Hints
- [x] `auto` deduced type hints
- [x] Structured binding type hints
- [x] Lambda return type hints
- [x] Range-based for loop variable type hints (via `auto` deduction)

### Designator Hints
- [ ] Aggregate initializer designators (`.field =`)

## Configuration

- [x] Range-scoped queries (only compute hints for visible range)
- [x] Position encoding support (UTF-16)

## Display

- [x] Left-anchored hints (parameter names before argument)
- [x] Right-anchored hints (types after variable name)
- [ ] Clickable/interactive hints
