# Hover

Implementation: `src/feature/hover.cpp` (ported from clangd's `Hover.cpp`, llvmorg-21.1.8)

## Symbol Information

- [x] Qualified name with scopes (`namespace_scope`, `local_scope`, `name`)
- [x] Symbol kind classification
- [x] Access specifier (public/protected/private)
- [x] Documentation comments (Doxygen)
- [x] Source definition rendering

## Type Information

- [x] Variable type with pretty-printing
- [x] Desugared type (`aka` field) for type aliases
- [x] Return type for functions/lambdas
- [x] Function parameters with types, names, defaults
- [x] Template parameters

## Layout Information

- [x] Size (bits) for fields and types
- [x] Offset within enclosing class
- [x] Padding detection
- [x] Alignment

## Expression Context

- [x] Evaluated constant value (`value` field)
- [x] Callee argument info (which parameter does this map to)
- [x] Pass-by semantics (ref, const ref, value)
- [x] Implicit conversion detection

## Module-Related

- [ ] Hover on `import` statement shows module info
- [ ] Hover on module name shows owning file/partition list

## Changelog

| Date    | Change                           | PR     |
| ------- | -------------------------------- | ------ |
| 2025-06 | Port clangd hover implementation | [#452] |

## References

[#452]: https://github.com/clice-io/clice/pull/452
