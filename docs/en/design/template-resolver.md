# Template Resolution

## Background

A core design principle of C++ templates is **deferred instantiation** -- before template code is instantiated with concrete type arguments, the compiler does not (and cannot) resolve names that depend on template parameters. These are called **dependent names**, and their types are unknown at template definition time.

For a language server, this means the interior of template code is a "blind spot":

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.  // cursor here -- vec's type is known to be vector<T>, completions can be provided
}
```

For simple cases, using the primary template's definition suffices to provide basic completions. But consider a more complex scenario:

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].  // What is the type of vec2[0]?
}
```

The type of `vec2[0]` is `std::vector<std::vector<T>>::reference` -- a dependent name. In standard library implementations, resolving it requires tracing through dozens of layers of nested template typedef chains: `reference` -> `allocator_traits<Alloc>::value_type` -> `__alloc_traits<Alloc>::reference` -> ... Each layer involves partial specialization matching, default template arguments, typedef expansion, and other complex operations.

In the clangd community, completion and hover issues in template code have persisted for a long time. Users repeatedly report the inability to get completion suggestions inside template function bodies -- the cursor position shows `<dependent type>` and all LSP features become non-functional. The root cause is that clangd's resolution strategy has several fundamental limitations:

**No partial specialization handling**: clangd assumes the primary template's definition is always used for member lookup. However, the standard library makes heavy use of partial specializations (e.g., `allocator_traits` specialized for different allocators), and the primary template may not even have the member being looked up -- the member only exists in a specific partial specialization.

**Missing argument mapping**: Even when name lookup finds a member's type, that type is still expressed in terms of the looked-up template's parameters (e.g., `Alloc` in `allocator_traits<Alloc>::value_type`), not the caller's parameters (e.g., `T`). Since clangd does not perform instantiation, it cannot establish mappings between parameters.

**Ignoring default template arguments**: `std::vector<T>` is actually `std::vector<T, std::allocator<T>>` -- the second argument is defaulted. clangd does not expand default arguments, causing names that depend on defaulted arguments to be unresolvable.

## Design

### Core Idea

clice implements a **PseudoInstantiator** -- it resolves dependent names through heuristic methods without requiring concrete type arguments. The core insight is: you do not need to know whether `T` is `int` or `string`; you only need to trace how `T` propagates through the template typedef chain and express the final result in terms of `T`.

For example: `std::vector<std::vector<T>>::reference` is simplified through pseudo-instantiation to `std::vector<T>&` -- a type concrete enough to provide completions while preserving the symbolic meaning of the template parameter `T`.

### Two-Phase Transform

The resolver is built on Clang's TreeTransform infrastructure and operates in two phases:

**Phase 1: PseudoInstantiator (Heuristic Resolution)**

This is the main engine, responsible for resolving various kinds of dependent names:

- **DependentNameType** (e.g., `typename Container::value_type`): looks up `value_type` among the template's members, matches partial specializations, expands typedefs, and re-expresses the result using the caller's parameters
- **DependentTemplateSpecializationType** (e.g., `Alloc::rebind<U>`): resolves dependent template specializations, with a fast path for standard library patterns tried first
- **TemplateTypeParmType** (e.g., `T`): looks up the parameter's bound value or default argument via the instantiation stack
- **DecltypeType** (e.g., `decltype(var)`): resolves simple variable references to their declared type

**Phase 2: SubstituteOnly (Cycle Breaking)**

When Phase 1 expands a typedef, it may encounter another dependent name requiring heuristic lookup, forming a cycle: typedef A's underlying type references dependent name B, and looking up B reveals its type involves typedef A again.

SubstituteOnly breaks such cycles -- it only performs argument substitution and typedef expansion without heuristic lookups. When Phase 1 needs to expand a typedef, it delegates to SubstituteOnly, ensuring that recursive heuristic lookups are never triggered.

### Instantiation Stack

Template parameters may exist at different depth levels in nested templates. The instantiation stack (InstantiationStack) maintains a stack of parameter mappings, with each frame recording one level of template parameter bindings.

When a `TemplateTypeParmType` (a template parameter identified by depth and index) is encountered, the resolver searches linearly from the top of the stack (innermost level) to the bottom (outermost level), matching parameter bindings at the corresponding depth. If a match is found, the parameter is substituted with the bound type; if not found, the parameter's default value is tried. When the stack is empty (a standalone dependent type), the resolver walks up through enclosing template declarations and pushes their injected template parameters as context.

This allows the resolver to handle multi-level nested templates:

```cpp
template<typename X>
struct Outer {
    template<typename Y>
    struct Inner {
        typename std::pair<X, Y>::first_type member;
        // X is at depth 0, Y is at depth 1
        // Both need to be tracked in the stack for correct resolution
    };
};
```

### Dependent Name Resolution Flow

Using `typename A<T>::type` as an example:

1. **Cache check**: If this node has been resolved before, return the cached result directly
2. **Cycle detection**: If this node is currently being resolved, abort to prevent infinite recursion
3. **Qualifier transform**: Transform the `A<T>` part -- substitute parameters, match partial specializations
4. **Member lookup**: Look up `type` in the transformed type. Try each partial specialization and its bases, then the primary template and its bases
5. **Argument deduction**: Use Clang's template argument deduction mechanism to establish a mapping from formal to actual parameters
6. **Substitution**: Expand typedefs in the found member type via SubstituteOnly, substituting with the caller's parameters
7. **Recursion**: If the result still contains dependent names, resolve recursively

The entire process has a recursion depth limit (16 levels) to prevent infinite recursion in extreme cases.

The resolver also maintains two layers of cycle detection: one prevents re-entrant resolution of the same DependentNameType node, and another prevents recursive lookup on the same ClassTemplateDecl (handling CRTP and other self-referential patterns).

### Partial Specialization Matching

Partial specialization matching is a key advantage of the pseudo-instantiator over clangd. The standard library makes heavy use of partial specializations, for example:

```cpp
// Primary template -- does not define value_type
template<typename Alloc> struct allocator_traits;

// Partial specialization -- defines value_type
template<typename T> struct allocator_traits<allocator<T>> {
    using value_type = T;
};
```

When resolving `allocator_traits<allocator<int>>::value_type`, clangd fails because it looks up `value_type` in the primary template. The pseudo-instantiator tries matching the actual arguments against each partial specialization's pattern, finds the correct specialization, and then looks up the member within it.

### Standard Library Special Handling

The standard library's allocator rebinding chain is a particularly deep typedef chain:

```text
vector<T> -> __alloc_traits<allocator<T>> -> allocator_traits<allocator<T>>
  -> allocator_traits<Alloc>::rebind_alloc<U>
```

This chain may exceed the depth limit. The resolver short-circuits the `allocator_traits::rebind_alloc` pattern: upon detecting it, the resolver directly tries `Alloc::rebind<T>::other` (the standard allocator protocol), or falls back to substituting the first template parameter.

### Type Resugaring

Internally, Clang represents template parameters as canonical `TemplateTypeParmType` values (depth + index). Users should see parameter names (e.g., `T`), not `parameter 0 of depth 0`. The ResugarOnly transform maps canonical parameter types back to their original declarations, producing user-friendly type information.

### Graceful Degradation

If any step in the resolution process fails (lookup failure, cycle detection triggered, depth limit exceeded), the resolver returns the original dependent type rather than reporting an error. This means LSP features are never completely broken due to a resolution failure -- they simply degrade to the "unresolvable" state, which is no worse than not performing pseudo-instantiation at all.

## Design Decisions and Trade-offs

**Why pseudo-instantiation instead of real template instantiation?** Real instantiation requires concrete type arguments (e.g., `T = int`), but this information is unavailable at template definition time. Moreover, code in a language server is frequently incomplete and cannot undergo full instantiation. Pseudo-instantiation avoids dependence on concrete types by preserving symbolic parameters (`T` itself).

**Why a two-phase design?** In a single-phase design, typedef expansion would trigger new heuristic lookups, and lookup results might in turn require typedef expansion -- forming a cycle. The two-phase design breaks this cycle through separation of concerns: heuristic lookups are performed only in Phase 1, and typedef expansion is delegated to Phase 2, which does not perform lookups.

**Why special-case standard library patterns?** The standard library's allocator rebinding is the most common deep typedef chain encountered in real-world code. Generic recursive resolution would exceed the depth limit here. While not elegant, the special-case handling covers the highest-frequency user scenarios. Future plans include replacing these special cases with a general mechanism.

**Why graceful degradation instead of error reporting?** Template resolution is inherently heuristic -- it is impossible to cover every edge case in C++ templates. Graceful degradation ensures that resolution failure is never worse than "not resolving at all," while providing a significant experience improvement in the cases where resolution succeeds.

## Known Limitations

- **Pack expansion**: Currently only single-element packs are handled (e.g., pack forwarding); multi-element pack expansion (e.g., `Us... = {int, float}`) is not supported.
- **Default values of non-type template parameters**: Only default value substitution for type template parameters (TemplateTypeParmDecl) is supported; default values for non-type parameters (e.g., `template<int N = 0>`) and template template parameters are not.
- **Dependent member expressions**: Dependent member access of the form `x.template foo<T>()` is not yet implemented.
- **Complex decltype**: Only the simple case `decltype(var)` is handled; member access, function calls, and other complex expressions are not supported.
- **Operator lookup**: Operators in dependent contexts (e.g., `a + b` where `a`'s type depends on a template parameter) cannot be resolved because operators lack identifier information.
