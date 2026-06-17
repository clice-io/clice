# Template Resolver

## The Problem

In C++, when a type depends on template parameters, its members cannot be resolved before instantiation. For language servers, this means losing functionality inside template code — no completion, no hover, no navigation.

Consider:

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.  // cursor here
}
```

clangd handles this by assuming the primary template, providing completion for `std::vector<T>`. But consider a more complex case:

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].  // cursor here
}
```

The type of `vec2[0]` is `std::vector<std::vector<T>>::reference`, which is a [dependent name](https://en.cppreference.com/w/cpp/language/dependent_name). In libstdc++, resolving this requires following dozens of nested template typedefs. clangd fails here because:

- Assumes primary templates but doesn't handle cases where partial specializations block lookup.
- Only performs name lookup without instantiation, so it can't map results back to original template parameters.
- Ignores default template parameters that introduce dependent names.

## Pseudo-Instantiation

clice implements a **pseudo-instantiator** (`src/semantic/resolver.cpp`) that resolves dependent types without concrete type arguments. It simplifies dependent names by following typedef chains and substituting template parameters symbolically.

For example: `std::vector<std::vector<T>>::reference` simplifies to `std::vector<T>&`, enabling full completion.

### Architecture

The resolver uses Clang's `TreeTransform` infrastructure with two phases:

1. **PseudoInstantiator** — performs heuristic lookup and substitution. Handles:
   - `DependentNameType` (e.g., `typename Container::value_type`)
   - `DependentTemplateSpecializationType`
   - `TemplateTypeParmType` (maps parameters back to their constraints)
   - Typedef chains through nested templates
   - Default template arguments

2. **SubstituteOnly** — a cycle-breaking fallback. If the instantiator detects a loop (type A depends on type B which depends on A), it switches to shallow substitution mode to avoid infinite recursion.

### Capabilities

- Resolves deeply nested dependent typedefs (handles libstdc++'s layers of indirection)
- Tracks template parameter mappings across multiple levels
- Handles `UnresolvedLookupExpr`, `UnresolvedUsingType`, `CXXUnresolvedConstructExpr`
- Caches results per-TU via `DenseMap<const void*, QualType>` for performance
- Provides `resugar()` to present user-friendly types (e.g., `string` instead of `basic_string<char, ...>`)

### User-Visible Impact

With the template resolver, clice provides code completion, hover information, and type deduction inside template bodies — even for complex standard library types that clangd cannot handle.
