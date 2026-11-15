# Template Resolution

## Background

Inside a template, a name that depends on a template parameter has no meaning until the template is instantiated: the compiler does not resolve it, and a language server sees a blind spot.

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> rows) {
    rows[0].  // what is rows[0]?
}
```

`rows[0]` is `std::vector<std::vector<T>>::reference`, a dependent name that the standard library defines through layers of aliases, partial specializations and default arguments: `reference` → `allocator_traits<Alloc>::value_type` → …. clangd users have long found completion and hover dead inside template bodies, for three reasons in how clangd resolves such names:

- **No partial specializations.** It looks members up in the primary template, which may not even declare them: `allocator_traits` defines them in its specializations.
- **No argument mapping.** A member found in `allocator_traits<Alloc>` is expressed in `Alloc`, not in the caller's `T`.
- **No default arguments.** `std::vector<T>` is `std::vector<T, std::allocator<T>>`; a name depending on the default stays unresolved.

## Design

### Pseudo-Instantiation

clice resolves dependent names without concrete arguments. It does not need to know whether `T` is `int` or `string`; it follows how `T` flows through the chain of aliases and expresses the result in `T`: `std::vector<std::vector<T>>::reference` becomes `std::vector<T>&`, concrete enough to complete members while keeping `T` symbolic.

The resolver is a type rewriter of its own. It uses neither Sema nor Clang's `TreeTransform`, which both assume a real instantiation with concrete arguments — exactly what is absent here. It runs under two policies: **resolve**, which looks names up — a dependent member, a dependent template, a parameter's binding or default, a `decltype` of a variable or a call — and **substitute**, which only replaces parameters and expands aliases. Resolving delegates alias expansion to substitution, so a lookup never starts another lookup through an alias that leads back to it.

### Parameters and Specializations

Nested templates bind parameters at several levels, kept as a stack of frames; a parameter is looked up in the frame of the template declaring it, so two templates whose parameters share a depth never capture each other's. A parameter with no binding takes its default.

To look a member up in `A<T>`, the resolver matches the arguments against each partial specialization's pattern with its own unification, picks the specialization, looks the member up there, then in the primary template and the bases, and maps the found type back onto the caller's arguments. This is what reaches `allocator_traits<allocator<T>>::value_type` where the primary template declares nothing.

The `type` of the standard library's type traits — `conditional`, `enable_if`, `type_identity`, the `remove_*` and `add_*` traits, `decay` — and the `value` of `conjunction`, `disjunction` and `negation` are answered by the rule the standard gives them: every library writes them through its own partial specializations, deducing through those would cost every query, and the answer is the same in all of them.

Results are **resugared**: shown with the parameters' names (`T`), not Clang's canonical "parameter 0 of depth 0".

### Degrading Gracefully

A step that fails — a lookup that finds nothing, a cycle, a budget exhausted — returns the dependent name as it was, never an error, so resolving is never worse than not resolving. Cycles are cut at three places: resolving a name already being resolved, looking up a name in a class template already being searched for it (CRTP), deducing an `auto` whose initializer needs its own type. A query also has a bound on its nesting and on its total steps.

## Instantiations as Implementations

Pseudo-instantiation reads the written template and says what a dependent name could mean. The unit's actual instantiations say what it does mean for each argument list. clice treats a template like an interface and its instantiations like implementations, as a virtual function and its overrides:

- **Semantic tokens** read instantiated code too: an instantiation reuses the template's locations, so its resolution lands on the token written in the template. Instantiations agreeing on a name's kind keep it, and the modifiers they share; disagreeing ones make it a `conflict`.
- **Hover and inlay hints** inside a function template the unit instantiates exactly once show that instantiation's arguments; with several, the parameters stay as written.
- **Navigation** reads every instantiation: the index keeps what each instantiation resolves in the template's text, so definition, references, call hierarchy and implementation on a dependent name list each resolution, in open and closed files alike.
- **Completion** parses only up to the cursor, before the code that calls the template, so a member access resolving no other way compiles the buffer once more and offers the members every instantiation has (of eight at most).

Features that emit one item per location — inlay hints, folds, the outline — skip instantiated code: an instantiation could only repeat or contradict what the template gives there.

## Design Decisions

- **Why not real instantiation?** It needs concrete arguments, which do not exist inside the template, and code being edited is often too incomplete to instantiate.
- **Why two policies?** If expanding an alias could start a lookup, and a lookup's result needed alias expansion, the two would recurse into each other. Substitution never looks anything up.
- **Why answer standard traits by rule?** They are the most common dependent types in real code and each library writes them differently; the standard fixes their answers.

## Known Limitations

The resolver works out what the written code determines and leaves the rest unresolved rather than guess:

- **Expressions on dependent operands.** A value expression folds once its operands are known (`N + 1` with `N` given), and matches a pattern spelled the same way (`trait<X, X + 1>` against `trait<N, N + 1>`); one it cannot prove equal stays dependent. The type of an operator expression on a dependent operand (`value + 1`) is not deduced; subscripts, `->` and calls through member operators resolve.
- **Choices the code leaves open.** A constraint depending on an unknown argument, two partial specializations neither more specialized, a base naming itself through `decltype`, and a `decltype` of a call that needs argument-dependent lookup leave the name dependent.
- **Overloads told apart by reference binding.** Among overloads differing in how they bind an argument or the object — `const U&&` against `const int&`, a template binding the object without `const` against a `const` member, an explicit object parameter — the resolver can choose another overload than the compiler, or none.
