# Semantic Tokens

Implementation: `src/feature/semantic_tokens.cpp`

## Token Types

Based on `SymbolKind` classification via `SemanticVisitor`:

- [x] Namespace
- [x] Type (class, struct, enum, typedef, type alias)
- [x] Variable (local, global, parameter, field)
- [x] Function (free function, method, constructor, destructor)
- [x] Macro
- [x] Enum member
- [x] Template parameter (type and non-type)
- [x] Concept
- [ ] Dependent name (unresolved using decl)

## Token Modifiers

- [x] Declaration vs reference distinction
- [x] Static modifier
- [x] Readonly/const
- [x] Deprecated (`[[deprecated]]`)
- [x] Abstract (pure virtual)
- [x] Virtual

## Encoding

- [x] UTF-16 position encoding (default)
- [x] Delta-encoded token positions (LSP spec)
- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [ ] Range-based semantic tokens (`textDocument/semanticTokens/range`)
- [ ] Delta updates (`textDocument/semanticTokens/full/delta`)

## Module-Related

- [x] Highlight `import`/`export`/`module` keywords distinctly
- [x] Highlight module names in import declarations
