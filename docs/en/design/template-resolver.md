# Template Resolver

First, there's better template support, which is also the feature I initially wanted clangd to support. Specifically, what problems are there currently in handling templates?

Take code completion as an example. Consider the following code, where `^` represents the cursor position:

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.^
}
```

In C++, if a type depends on template parameters, we cannot make any accurate assumptions about it before template instantiation. For example, here `vector` could be either the primary template or the partial specialization of `vector<bool>`. Which one should we choose? For code compilation, accuracy is always the most important - we cannot use any results that might lead to errors. But for language servers, providing more possible results is often better than providing nothing. We can assume that users use the primary template more often than partial specializations, and thus provide code completion results based on the primary template. Currently, clangd does exactly this - in the above case, it will provide code completion based on the primary template of `vector`.

Consider a more complex example:

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].^
}
```

From the user's perspective, completion should also be provided here, since the type of `vec2[0]` is also `vector<T>`, right? Same as the previous example. But clangd won't provide any completion here. What's the problem? According to the C++ standard, the return type of `std::vector<T>::operator[]` is `std::vector<T>::reference`, which is actually a [dependent name](https://en.cppreference.com/w/cpp/language/dependent_name). Its result seems quite direct - it's `T&`. But in libstdc++, its definition is nested in dozens of layers of templates, seemingly for compatibility with old standards? So why can't clangd handle this situation?

1. It's based on primary template assumptions, not considering that partial specializations might make lookup impossible to proceed
2. It only performs name lookup without template instantiation, so even if it finds the final result, it can't map it back to the original template parameters
3. It doesn't consider default template parameters, unable to handle dependent names caused by default template parameters

Although we can make exceptions for standard library types to provide related support, I hope that user code can have the same status as standard library code, so we need a universal algorithm to handle dependent types. To solve this problem, I wrote a pseudo-instantiation (pseudo instantiator). It can instantiate dependent types without specific types, achieving the purpose of simplification. For example, in the above example, `std::vector<std::vector<T>>::reference` can be simplified to `std::vector<T>&`, and further provide code completion options for users.

## How It Works

The `TemplateResolver` class takes a reference to Clang's `Sema` and resolves dependent types through pseudo-instantiation. The core mechanism is a `PseudoInstantiator` class built on Clang's `TreeTransform` framework. It works in two phases:

**Phase 1 — Heuristic lookup (`PseudoInstantiator`).** Walks the type tree and transforms dependent constructs:

- `TransformDependentNameType`: looks up the member name in the primary template of the qualifier, substitutes template parameters, and recurses into the result.
- `TransformDependentTemplateSPTType`: resolves dependent template specialization types by looking up the template name and deducing arguments.
- `TransformTemplateTypeParmType`: substitutes template type parameters from an `InstantiationStack` that tracks the current pseudo-instantiation context, with fallback to default template arguments.
- `TransformTypedefType`: delegates to Phase 2 to expand typedefs without triggering heuristic lookup.

**Phase 2 — Substitution only (`SubstituteOnly`).** A separate `TreeTransform` that expands typedefs and substitutes template parameters from the stack, but does NOT override `TransformDependentNameType`. This is critical: it prevents infinite cycles where typedef expansion triggers a lookup that expands back into the same typedef. Phase 2 handles `TypedefType`, `ElaboratedType`, `InjectedClassNameType`, alias template specializations, and `TemplateTypeParmType`.

A separate "resugar" pass performs a simpler transform: it walks the type tree and replaces canonical template type parameter nodes (which lack source-level names) with their original named declarations, restoring readability for display purposes (e.g. showing `T` instead of `type-parameter-0-0`).

## The Lookup Mechanism

The lookup mechanism resolves a nested name specifier chain to a concrete declaration context and performs name lookup there. Different overloads handle different kinds of dependent AST nodes:

- Dependent name types (e.g. `typename T::value_type`)
- Dependent template specializations (e.g. `typename T::template rebind<U>`)
- Dependent scope references in expressions (e.g. `T::value`)
- Unresolved lookup expressions and using declarations

## Caching

Resolved types are cached by AST node pointer. Since each AST node has a unique identity within a translation unit, the cache is safe to share across multiple resolution calls on the same TU. The cache is checked before performing expensive tree transforms, avoiding redundant resolution of the same dependent type node.
