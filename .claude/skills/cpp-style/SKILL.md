---
name: cpp-style
description: clice C++ coding conventions — redundancy elimination (the rule we care most about), file organization, error/defense policy, template deduction and type-trait rules, naming, logging, modern C++/LLVM API preferences. Read BEFORE writing or modifying any C++ code.
---

# clice C++ Coding Style

## Redundancy Is a Defect

This is the convention we care most about. Redundant code is not a style
nit — it actively misleads: every guard implies the guarded state can
occur, every branch implies it can be taken, every parameter implies a
caller needs it. When that implication is false, the reader wastes time
defending against ghosts.

- **Every branch must be reachable.** Before adding a guard or fallback,
  prove the state can actually occur — construct the input that hits it.
  If you can't, don't write it.
- **No speculative generality.** No parameters, options, hooks, or
  abstraction layers for hypothetical future callers. Add them when the
  second real caller arrives.
- **One way to do each thing.** Don't leave an old path alive next to its
  replacement "just in case" — migrate all callers and delete it in the
  same change.
- **Re-read after every change.** Edits leave residue: conditions that
  became constant, variables read once, branches that now collapse,
  `else` after `return`, a helper with one remaining caller. Fold them
  before you're done — simplification that removes a concept beats one
  that merely shortens lines.
- **Delete, don't comment out.** Git history is the archive.

## Files & Organization

- Headers are `.h` with `#pragma once` — never include guards.
- Sources are `.cpp`; entry points are `.cc` (`clice.cc`,
  `src/driver/*.cc`) — a deliberate distinction, revisit when the project
  moves to C++20 modules.
- File names are `snake_case`.
- File-local helpers: a single one is `static`; a cluster of them goes in
  one anonymous namespace.

## Data & Types

- `struct` by default, even for types with methods. `class` only when
  there is a real invariant that private access protects.
- `enum class` with an explicit underlying type (e.g. `: std::uint8_t`);
  document each enumerator with `///` when its meaning is not obvious.
- Prefer designated initializers (`{.field = value}`) for aggregate
  construction.
- West const: `const T&`, never `T const&`.

## Errors, Exceptions & Defense

- The project builds with `-fno-exceptions -fno-rtti`: no
  `throw`/`try`/`catch`, no `dynamic_cast`/`typeid`. Polymorphic
  inspection uses `llvm::isa<>` / `cast<>` / `dyn_cast<>`.
- Fallible synchronous operations return `std::expected<T, E>` with `E` =
  `std::string` or `std::error_code`; async code uses kota's outcome
  types. `llvm::Error` only where an LLVM API forces it.
- `assert` pins preconditions; impossible branches end in
  `std::unreachable()` — never `llvm_unreachable` (project convention).
- Invalid external input (broken source code, malformed requests)
  degrades gracefully — empty result, input passed through — it never
  crashes the server. But graceful degradation is not scattered null
  checks: per the redundancy rule, defend only against states that can
  occur. The Debug (ASan + assertions) test surface is the safety net.

## Error Handling (control flow)

- **Prefer `if` with init-statements to tightly scope error variables**, but avoid them when they compromise code readability or flatten control flow.
- **Omit redundant conditions:** If the error type provides an `operator bool` or evaluates implicitly (e.g., standard error codes, custom error wrappers), omit the redundant condition check.
- **Avoid forced `else` branches:** If scoping the variable inside the `if` requires you to introduce an `else` block for the success path (especially when returning early on error), declare the variable in the local scope instead to keep the control flow flat.

```cpp
// Good: Omit redundant condition when the type has operator bool
if (auto err = foo()) {
    /* handle error */
}

// Bad: Redundant condition check
if (auto err = foo(); err) {
    /* handle error */
}

// Good: Use init-statement when a custom condition is required,
// AND the variable isn't needed outside the if-statement
if (auto result = foo(); !result.has_value()) {
    /* handle error */
}

// --- Scope and Control Flow Considerations ---

// Bad: Using init-statement forces an 'else' block because 'result'
// goes out of scope, leading to nested/redundant code.
if (auto result = get_data(); !result.has_value()) {
    return result.error();
} else {
    process(result.value()); // Success path is forced into a nested block
}

// Good: Declare as a regular local variable to allow early exit
// and keep the success path un-nested (flat control flow).
auto result = get_data();
if (!result.has_value()) {
    return result.error();
}
process(result.value());
```

## Concurrency & Async

- Async code is kota coroutines (`kota::task`, `co_await`) — no callback
  style. A public interface may stay synchronous and drive a coroutine
  internally when the caller has no event loop (see `Toolchain`).

## Logging

- Log through the `LOG_TRACE` / `LOG_DEBUG` / `LOG_INFO` / `LOG_WARN` /
  `LOG_ERR` macros (`std::format` syntax, source location captured
  automatically); `logging::critical` logs and aborts. Never call spdlog
  directly, never print diagnostics to stdout/stderr.

## Naming Conventions

- **Variables, member fields, function names**: `snake_case`. Class member fields do NOT use any special suffix/prefix (no trailing `_`, no `m_` prefix).
- **Class names, template parameter names, enum names**: `PascalCase`. Exception: some class names also use `snake_case` — follow the existing style in the project.
- **Enum values**: `PascalCase`.
- Doc comments on declarations use `///`; the bar for when to write a
  comment at all is in CLAUDE.md.

## Template & Type Traits

- Do NOT blindly add `std::remove_cvref_t` on every template parameter. Understand C++ template argument deduction rules:
  - `template<typename T> void f(T x)` — `T` is always deduced as a non-reference, non-cv-qualified type. No need for `remove_cvref_t`.
  - `template<typename T> void f(T& x)` — `T` is deduced as the referred-to type (possibly cv-qualified, but never a reference). No need for `remove_cvref_t` to strip references.
  - `template<typename T> void f(const T& x)` — `T` is deduced as a non-const, non-reference type. No need for `remove_cvref_t`.
  - `template<typename T> void f(T&& x)` — **forwarding reference**: `T` CAN be deduced as an lvalue reference (e.g., `int&`). This is the ONLY case where `std::remove_cvref_t<T>` is needed to get the bare type.
  - Class template parameters and return types are also never deduced as references; don't add `remove_cvref_t` on them either.

## Type Traits & Concepts (C++20/23)

- This project targets C++20/23. Use variable templates directly for type traits — do NOT use the old pattern of wrapping a class template static member in a variable template. Prefer:

  ```cpp
  // Good: directly specialize a variable template
  template<typename T>
  inline constexpr bool is_my_type_v = false;

  template<>
  inline constexpr bool is_my_type_v<MyType> = true;
  ```

  ```cpp
  // Bad: unnecessary class template wrapper
  template<typename T>
  struct is_my_type : std::false_type {};

  template<>
  struct is_my_type<MyType> : std::true_type {};

  template<typename T>
  inline constexpr bool is_my_type_v = is_my_type<T>::value;
  ```

- When defining a concept that checks a type trait, do NOT add `std::remove_cvref_t` unless you specifically intend the concept to see through references/cv-qualifiers. If the concept is meant for a bare type, just use `T` directly — the caller is responsible for passing the right type.

  ```cpp
  // Good
  template<typename T>
  concept MyTrait = is_my_type_v<T>;

  // Bad: unnecessary remove_cvref_t
  template<typename T>
  concept MyTrait = is_my_type_v<std::remove_cvref_t<T>>;
  ```

## String Literals

- Prefer C++11 raw string literals `R"(...)"` over escaped strings. Avoid `\"`, `\\`, `\n` in string literals when a raw literal is cleaner.

## Style

- Prefer `[[maybe_unused]]` over `(void)` for intentionally unused variables or parameters.

## Modern C++ Usage

- Use C++20/23 APIs whenever possible. Do NOT use `<iostream>` facilities (`std::cout`, `std::cin`, `std::cerr`, etc.). Also do NOT use C-style I/O (`printf`, `fprintf`, etc.).
- Prefer `std::ranges` / `std::views` APIs over raw loops and traditional `<algorithm>` calls.
- Prefer LLVM's efficient data structures (e.g., `llvm::SmallVector`, `llvm::DenseMap`, `llvm::StringMap`, `llvm::StringRef`) over their `std` counterparts when appropriate.

## Parameter Passing Preferences

- For string parameters, prefer `llvm::StringRef` > `std::string_view` > `const std::string&`.
- For array/span parameters, prefer `llvm::ArrayRef` > `std::span` > `const std::vector&`.
