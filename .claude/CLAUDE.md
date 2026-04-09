# clice — Project Guide

## Project Overview

clice is a next-generation C++ language server (LSP) built on LLVM/Clang, targeting modern C++ (C++20/23). It uses a multi-process architecture with a master server coordinating stateless and stateful workers.

## Core Correction Patterns — Lessons from Past Interactions

The following patterns were extracted from extensive real-world collaboration. These are recurring mistakes that MUST be avoided. Read them carefully — they represent hard-won lessons, not hypothetical concerns.

### Pattern 1: Misjudging Real-World Priorities

AI tends to optimize whatever metric looks most impressive, rather than what actually matters in the user's real scenario.

**Example**: During performance optimization, the AI proudly reported "hot cache is 4-7x faster!" — but the function in question runs at LSP server startup, which is ALWAYS a cold start. Optimizing hot cache was completely meaningless.

**Rule**: Before optimizing or analyzing anything, first understand the REAL usage scenario. Ask yourself: "When does this code actually run? What does the user actually experience?" Do not chase metrics that look good on paper but are irrelevant in practice.

### Pattern 2: Pushing Without Local Verification

The most common and most damaging pattern. AI proposes a fix, pushes it immediately, CI fails, then another fix, push, fail again — wasting CI cycles and the user's time.

**Rule**: NEVER push code that you haven't verified locally. Before every push:

- Build locally with the same configuration CI uses.
- Run the relevant tests locally and confirm they pass.
- If you cannot reproduce the CI environment locally, say so — do not just "try and see."
- "It compiles" is NOT sufficient. Tests must pass.

### Pattern 3: Superficial Refactoring

When asked to refactor, AI tends to do mechanical code movement (copy functions from A to B) without understanding the deeper design intent (ownership, responsibility boundaries, API cleanliness).

**Example**: When splitting `MasterServer` into `Workspace` and `Session`, the AI moved functions but kept ugly APIs like `f(path_id, sessions_map)` instead of the clean `f(Session&)` that the refactoring was meant to achieve.

**Rule**: When refactoring, understand the WHY. Ask: "What design problem is this refactoring solving?" If you're just moving code around without improving the abstractions, you're not refactoring — you're rearranging deck chairs.

### Pattern 4: Fixing Only the Immediate Instance, Not the Pattern

When given a cleanup instruction, AI applies it to the single file or function currently being discussed, ignoring all other occurrences in the project.

**Example**: User says "remove decorative `===` comment separators." AI removes them from `workspace.cpp` only. User has to say: "The other files too!"

**Rule**: When given a cleanup or style instruction, apply it project-wide. Use `Grep` to find ALL occurrences and fix them all in one pass. Think: "Where else does this pattern appear?"

### Pattern 5: Never Skip, Disable, or Work Around Failing Tests

When stuck on a difficult bug (especially flaky CI, race conditions, platform-specific issues), AI may propose marking tests as `continue-on-error`, skipping them, or adding `expected-failure` annotations to make CI green.

**Rule**: This is ABSOLUTELY FORBIDDEN. If a test fails, fix the root cause. There are ZERO exceptions. Skipping a test to make CI green is not "fixing" — it is hiding a bug. If you ever find yourself thinking "maybe we should just skip this test," stop and reconsider your approach entirely.

### Pattern 6: Excessive Confirmation Seeking vs. Premature Execution

AI oscillates between two extremes: asking "should I do X?" for every trivial decision, or silently executing major changes without confirmation.

**Rule**: Calibrate based on reversibility and impact:

- **Small, reversible changes** (formatting, renaming a local variable, adding a test): just do it.
- **Architecture decisions, API changes, large refactors**: propose the plan first, wait for confirmation.
- **Pushing to remote, creating PRs, modifying CI**: always confirm.
- When the user says "go ahead" or "do it," execute fully without asking again mid-way.

## Code Reuse & Understanding Before Implementation

**This is the single most important rule in this project.** Before writing ANY new code, you MUST thoroughly read and understand the existing codebase first. This project has a rich set of utilities, abstractions, and patterns already in place — duplicating them wastes effort and creates maintenance burden.

Concrete requirements:

1. **Read before you write.** Before implementing a feature or fix, explore the relevant modules in `src/`. Search for existing helpers, utilities, and patterns that solve the same or similar problems. Use `Grep`, `Glob`, and `Agent` tools to investigate thoroughly — do not assume something doesn't exist just because you haven't seen it yet.

2. **Reuse existing infrastructure.** This project already has:
   - A `Lexer` class (`src/syntax/lexer.h`) — do not hand-write token scanning logic.
   - A `PositionMapper` for source location conversion — do not reimplement offset-to-line/column math.
   - `CompilationUnitRef` methods (`decompose_location`, `decompose_range`, `file_path`, `directives`, etc.) — use them instead of raw Clang APIs.
   - `SemanticVisitor` for AST traversal — extend it, do not write custom recursive AST walkers.
   - `Tester` framework for unit tests with VFS, annotation support, and multi-phase compilation — use it, do not create ad-hoc test setups.
   - Utility functions in `src/support/` — check there before writing new helpers.

3. **Follow established patterns.** When adding a new feature (e.g., a new LSP request handler), look at how 2-3 existing features of the same kind are implemented. Match their structure: same file organization, same function signatures, same error handling patterns. If every other feature in `src/feature/` follows a certain pattern, yours should too.

4. **Do not reinvent what the project already has.** If you find yourself writing a helper function that feels generic (string manipulation, path handling, JSON serialization, source range conversion), STOP and search the codebase first. There is a high probability it already exists. Creating duplicates leads to inconsistencies and bugs when one copy gets updated but the other doesn't.

5. **When in doubt, ask.** If you're unsure whether an existing utility covers your use case or whether to extend an existing abstraction vs. create a new one, ask the user rather than guessing.

## Source Layout

- `src/server/` — LSP server core: master server, compiler, indexer, stateful/stateless workers
- `src/feature/` — LSP feature implementations: hover, completion, document links, semantic tokens, etc.
- `src/compile/` — Compilation orchestration: compilation unit, directives, diagnostics
- `src/index/` — Symbol indexing: TUIndex, ProjectIndex, MergedIndex, include graph
- `src/semantic/` — Semantic analysis: symbol kinds, relations, AST visitor, template resolver
- `src/syntax/` — Lexer, scanner, token types, dependency graph
- `src/command/` — CLI parsing, compilation database, toolchain detection
- `src/support/` — Utilities: logging, filesystem, JSON, string helpers

## Build System

- Uses **pixi** for environment management and **CMake + Ninja** for building.
- Two build types: `Debug` and `RelWithDebInfo` (default).
- Build output goes to `build/[type]/`.
- See `/build`, `/test`, `/format` commands for common operations.

## Commit Message Format

Use **conventional commits** — enforced by CI:

```
<type>(<scope>): <short description>
```

- **Types**: `feat`, `fix`, `refactor`, `chore`, `docs`, `ci`, `test`
- **Scopes**: match `src/` subdirectories or feature names, e.g. `completion`, `server`, `index`, `tests`, `document links`
- Keep the subject line under 70 characters.

## Tests

Three types of tests, all must pass before committing:

- **Unit tests** (`tests/unit/`): C++ tests using the project's own test framework. Test names should be at most 4 words.
- **Integration tests** (`tests/integration/`): Python pytest tests that start a real clice server and communicate via LSP.
- **Smoke tests** (`tests/smoke/`): Replay recorded LSP sessions via `tests/replay.py`.

### Integration Test Style

- Keep tests concise. Do NOT write large comment blocks explaining the test layout or expected behavior.
- Use descriptive test function names and short inline comments only where logic is non-obvious.

## Pre-PR Review

Before opening a PR, launch **3 parallel subagents** to review the diff independently:

1. **Correctness reviewer**: Check for logic errors, edge cases, undefined behavior, and off-by-one mistakes.
2. **Style reviewer**: Verify the code follows this project's naming conventions, coding style, and CLAUDE.md rules.
3. **Test reviewer**: Confirm test coverage is adequate — new functionality has tests, edge cases are covered, and no existing tests were broken or weakened.

Each agent should read the full diff (`git diff main...HEAD`) and report issues. Fix all reported issues before opening the PR.

## Pre-commit Checklist

Before committing code, you MUST:

1. **Run `pixi run format`** to format all source files.
2. **Pass all three types of tests:**
   - Unit tests: `pixi run unit-test [type]`
   - Integration tests: `pixi run integration-test [type]`
   - Smoke tests: `pixi run smoke-test [type]`
3. **All test failures must be fixed before committing.** This is a HARD REQUIREMENT with NO exceptions:
   - If a test fails, it MUST be fixed before you commit. Do NOT commit with known failures.
   - Do NOT skip, disable, or mark tests as expected-failure to work around breakage.
   - Do NOT argue "this test was already broken before my changes" — if it fails on your branch, it is YOUR responsibility to fix it before committing. The main branch CI is green; any failure on your branch is caused by your changes, period.
   - Do NOT defer fixing to a follow-up PR. Fix it NOW, in this branch, before committing.

---

## C++ Coding Style

### Template & Type Traits

- Do NOT blindly add `std::remove_cvref_t` on every template parameter. Understand C++ template argument deduction rules:
  - `template<typename T> void f(T x)` — `T` is always deduced as a non-reference, non-cv-qualified type. No need for `remove_cvref_t`.
  - `template<typename T> void f(T& x)` — `T` is deduced as the referred-to type (possibly cv-qualified, but never a reference). No need for `remove_cvref_t` to strip references.
  - `template<typename T> void f(const T& x)` — `T` is deduced as a non-const, non-reference type. No need for `remove_cvref_t`.
  - `template<typename T> void f(T&& x)` — **forwarding reference**: `T` CAN be deduced as an lvalue reference (e.g., `int&`). This is the ONLY case where `std::remove_cvref_t<T>` is needed to get the bare type.
  - Class template parameters and return types are also never deduced as references; don't add `remove_cvref_t` on them either.

### Type Traits & Concepts (C++20/23)

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

### Naming Conventions

- **Variables, member fields, function names**: `snake_case`. Class member fields do NOT use any special suffix/prefix (no trailing `_`, no `m_` prefix).
- **Class names, template parameter names, enum names**: `PascalCase`. Exception: some class names also use `snake_case` — follow the existing style in the project.
- **Enum values**: `PascalCase`.

### String Literals

- Prefer C++11 raw string literals `R"(...)"` over escaped strings. Avoid `\"`, `\\`, `\n` in string literals when a raw literal is cleaner.

### Error Handling

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

### Style

- Prefer `[[maybe_unused]]` over `(void)` for intentionally unused variables or parameters.

### Modern C++ Usage

- Use C++20/23 APIs whenever possible. Do NOT use `<iostream>` facilities (`std::cout`, `std::cin`, `std::cerr`, etc.). Also do NOT use C-style I/O (`printf`, `fprintf`, etc.).
- Prefer `std::ranges` / `std::views` APIs over raw loops and traditional `<algorithm>` calls.
- If the project depends on LLVM, prefer LLVM's efficient data structures (e.g., `llvm::SmallVector`, `llvm::DenseMap`, `llvm::StringMap`, `llvm::StringRef`) over their `std` counterparts when appropriate.

### Parameter Passing Preferences

- For string parameters, prefer `llvm::StringRef` > `std::string_view` > `const std::string&`.
- For array/span parameters, prefer `llvm::ArrayRef` > `std::span` > `const std::vector&`.
