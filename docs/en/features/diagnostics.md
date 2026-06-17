# Diagnostics

## Core

- [x] Clang diagnostics (errors, warnings, notes)
- [x] Severity mapping (Error, Warning)
- [x] Diagnostic ranges with source locations
- [x] Related information (notes attached to diagnostics)
- [x] File URI conversion for cross-file diagnostics
- [ ] Pull diagnostics model (`textDocument/diagnostic`) ([clangd#2108](https://github.com/clangd/clangd/issues/2108))
- [ ] Report all missing `#include` errors, not just the first — the parser stops on the first fatal error

  ```cpp
  #include "missing_a.h"  // error reported
  #include "missing_b.h"  // error NOT reported (parser already stopped)
  #include "missing_c.h"  // error NOT reported
  ```

- [ ] Show the full include chain for diagnostics originating in header files ([clangd#1392](https://github.com/clangd/clangd/issues/1392))

  ```
  // current:  "In included file: expected ';'"
  // expected: "In main.cpp → utils.h → detail/impl.h: expected ';'"
  ```

- [ ] Reflect unsaved header file changes in diagnostics ([clangd#488](https://github.com/clangd/clangd/issues/488))

  ```
  // header.h (unsaved buffer): added new_func()
  // main.cpp: calls new_func() → should NOT show "undeclared identifier"
  ```

- [ ] Diagnostics for template instantiation errors in preamble headers ([clangd#137](https://github.com/clangd/clangd/issues/137))

## Tags

- [x] `Deprecated` tag for `-Wdeprecated` diagnostics
- [x] `Unnecessary` tag for unused variable/parameter warnings

## Publishing

- [x] Push diagnostics on compilation completion
- [x] Clear diagnostics on file close
- [x] Per-file diagnostic grouping (interested file + headers)
- [ ] Configurable debounce delay before computing diagnostics ([clangd#1471](https://github.com/clangd/clangd/issues/1471))
- [ ] Recompute diagnostics in open files when background indexing completes ([clangd#2604](https://github.com/clangd/clangd/issues/2604))

## Diagnostic Suppression

- [ ] `// NOLINT` comment suppression
- [ ] `// NOLINTNEXTLINE` comment suppression
- [ ] `// NOLINTBEGIN` / `// NOLINTEND` block suppression
- [ ] `NOLINT` for include-cleaner diagnostics ([clangd#1982](https://github.com/clangd/clangd/issues/1982))
- [ ] Configurable severity per diagnostic category in config file ([clangd#1937](https://github.com/clangd/clangd/issues/1937))
- [ ] Filter diagnostics by version control diff — only show warnings near changed lines ([clangd#822](https://github.com/clangd/clangd/issues/822))

## Header Diagnostics

- [ ] Suppress false `-Wunused-function` for static inline functions in headers ([clangd#1211](https://github.com/clangd/clangd/issues/1211))

  ```cpp
  // utils.h
  static inline int helper() { return 42; }
  // should NOT warn "unused function" when checking header standalone
  ```

- [ ] Propagate `-Wpadded` and similar layout warnings from headers ([clangd#1429](https://github.com/clangd/clangd/issues/1429))
- [ ] Suppress false `-Wempty-translation-unit` from preamble optimization ([clangd#2358](https://github.com/clangd/clangd/issues/2358))
- [ ] Thread safety analysis across header boundaries ([clangd#2386](https://github.com/clangd/clangd/issues/2386))

## clang-tidy Integration

- [ ] clang-tidy diagnostics (gated by config)
- [ ] Suppress clang-tidy warnings originating in system-header macros ([clangd#1587](https://github.com/clangd/clangd/issues/1587), [clangd#2000](https://github.com/clangd/clangd/issues/2000))
- [ ] Clang static analyzer support ([clangd#905](https://github.com/clangd/clangd/issues/905))
- [ ] Version-specific clang-tidy documentation links ([clangd#2136](https://github.com/clangd/clangd/issues/2136))
- [ ] Diagnostics for preprocessor directives that precede code ([clangd#2501](https://github.com/clangd/clangd/issues/2501))

## Diagnostic Display

- [ ] Correct diagnostic range for qualified type names — underline the full name, not just the base ([clangd#1035](https://github.com/clangd/clangd/issues/1035))

  ```cpp
  ns::Inner obj(42);
  //  ^^^^^ only base name underlined
  // should underline "ns::Inner"
  ```

- [ ] Show optimization remarks (`-Rpass`) as diagnostics ([clangd#2519](https://github.com/clangd/clangd/issues/2519))
- [ ] Project-wide warning listing ([clangd#1973](https://github.com/clangd/clangd/issues/1973))

## Config File Diagnostics

- [ ] Report diagnostics for malformed `.clang-tidy` and `.clang-format` files ([clangd#2313](https://github.com/clangd/clangd/issues/2313), [clangd#2591](https://github.com/clangd/clangd/issues/2591))

## Diagnostic Correctness

Known issues that should be handled correctly:

- [ ] Macro redefinition warning pointing to the same location ([clangd#2479](https://github.com/clangd/clangd/issues/2479))
- [ ] Wrong `-Wmissing-braces` fix suggestion for nested array initialization ([clangd#2434](https://github.com/clangd/clangd/issues/2434))
- [ ] Invalid severity 0 when preamble invalidation mixes diagnostic messages ([clangd#2124](https://github.com/clangd/clangd/issues/2124))
- [ ] Undeclared identifier diagnostic hidden by correction-related diagnostics ([clangd#547](https://github.com/clangd/clangd/issues/547))
- [ ] Misleading downstream diagnostics when `--include` file is missing ([clangd#2229](https://github.com/clangd/clangd/issues/2229))

## Changelog

| Date | Change | PR |
| ---- | ------ | -- |
| —    | Clang diagnostics, severity mapping, tags, push publishing | — |
