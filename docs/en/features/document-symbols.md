# Document Symbols

Implementation: `src/feature/document_symbols.cpp`

## Symbol Hierarchy

- [x] Nested document symbol tree (parent-child relationships)
- [x] Symbol kind mapping from `SymbolKind`
- [x] Symbol ranges and selection ranges
- [x] UTF-16 position encoding

## Symbol Kinds Reported

- [x] Namespace
- [x] Class / Struct / Union
- [x] Enum / Enum member
- [x] Function / Method / Constructor
- [x] Variable / Field / Binding
- [ ] Typedef / Type alias
- [x] Template declarations (via inner templated entity)
