# Diagnostics

## Core

- [x] Clang diagnostics (errors, warnings, notes)
- [x] Severity mapping (Error, Warning)
- [x] Diagnostic ranges with source locations
- [x] Warnings raised by directives in the preamble (`#warning`, `#pragma message`, macro redefinitions)
- [x] File URI conversion for cross-file diagnostics
- [x] Pull diagnostics model (`textDocument/diagnostic`) for clients that declare support for it; other clients get their diagnostics pushed
- [x] Every missing `#include` is reported, not just the first, and the code after it is still checked
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

- [x] Diagnostics for template instantiation errors in preamble headers ([clangd#137](https://github.com/clangd/clangd/issues/137))

## Errors in Included Headers

An error inside a header is reported on the `#include` that brings it in, so it shows in the file you are editing.

<!-- BEGIN GENERATED ITEMS: diagnostics/headers -->

<!-- BEGIN CAPABILITY: supported -->

**Errors in included headers**

An error inside an included header shows on the line that includes it

The first error of each included header is reported on its `#include` line, with a note at the place in the header where it occurred. The header's other errors belong to the header and show when it is opened.

```case
tests/diagnostics/headers/01_errors_on_include
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Template Instantiations

An error inside a template is reported on the code that instantiates it.

<!-- BEGIN GENERATED ITEMS: diagnostics/templates -->

<!-- BEGIN CAPABILITY: supported -->

**Errors in template instantiations**

An error inside a template shows where the code asks for the instantiation

When a template fails for the arguments it is instantiated with, the error is reported on the code that requested the instantiation, not inside the template. Its notes lead through the chain of instantiations to the place in the template where it failed.

```case
tests/diagnostics/templates/01_instantiation_errors
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Notes

The notes clang attaches to an error travel with it as related information.

<!-- BEGIN GENERATED ITEMS: diagnostics/notes -->

<!-- BEGIN CAPABILITY: supported -->

**Notes as related information**

The notes clang attaches to an error point to the places they name

An error comes with the notes that explain it, such as where a name was first defined or which candidate a call could not use. Each note is related information of the error, so an editor shows it beside the error and jumps to its place.

```case
tests/diagnostics/notes/01_related_information
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Files Without a Compilation Database

A file the compilation database does not list still compiles, with a command clice guesses.

<!-- BEGIN GENERATED ITEMS: diagnostics/guidance -->

<!-- BEGIN CAPABILITY: supported -->

**No compilation database**

A file no compilation database lists compiles with a guessed command and says so

Without a `compile_commands.json` entry, a file still compiles, with a command clice guesses. When that leaves an include unresolved, a warning at the top of the file explains why and links to the setup guide. Once the database lists the file, the warning goes, and the errors left are the code's own.

```case
tests/diagnostics/guidance/01_no_database
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Tags

- [x] `Deprecated` tag for `-Wdeprecated` diagnostics and `modernize-*` clang-tidy findings
- [x] `Unnecessary` tag for unused variable/parameter warnings and `misc-unused-*` clang-tidy findings

## Publishing

- [x] Push diagnostics on compilation completion
- [x] Clear diagnostics on file close
- [x] Per-file diagnostic grouping (main file + headers)
- [x] Diagnostic `code` field with Clang error codes
- [x] `codeDescription` linking a clang-tidy finding to its check's documentation
- [ ] `codeDescription` with links to Clang documentation
- [x] Diagnostic `source` field distinguishing clang vs clang-tidy
- [ ] Configurable debounce delay before computing diagnostics ([clangd#1471](https://github.com/clangd/clangd/issues/1471))
- [ ] Recompute diagnostics in open files when background indexing completes ([clangd#2604](https://github.com/clangd/clangd/issues/2604))

## Diagnostic Suppression

- [x] `// NOLINT` comment suppression
- [x] `// NOLINTNEXTLINE` comment suppression
- [x] `// NOLINTBEGIN` / `// NOLINTEND` block suppression
- [ ] `NOLINT` for include-cleaner diagnostics ([clangd#1982](https://github.com/clangd/clangd/issues/1982))
- [ ] Configurable severity per diagnostic category in config file ([clangd#1937](https://github.com/clangd/clangd/issues/1937))
- [ ] Filter diagnostics by version control diff — only show warnings near changed lines ([clangd#822](https://github.com/clangd/clangd/issues/822))

## Diagnostic Actions

- [x] Fixes the compiler and clang-tidy attach to their diagnostics, offered as quick fixes

## Header Diagnostics

- [ ] Include-cleaner diagnostics for unused and missing `#include` directives
- [x] Suppress false `-Wunused-function` for static inline functions in headers ([clangd#1211](https://github.com/clangd/clangd/issues/1211))

  ```cpp
  // utils.h
  static inline int helper() { return 42; }
  // should NOT warn "unused function" when checking header standalone
  ```

- [ ] Propagate `-Wpadded` and similar layout warnings from headers ([clangd#1429](https://github.com/clangd/clangd/issues/1429))
- [ ] Suppress false `-Wempty-translation-unit` from preamble optimization ([clangd#2358](https://github.com/clangd/clangd/issues/2358))
- [ ] Thread safety analysis across header boundaries ([clangd#2386](https://github.com/clangd/clangd/issues/2386))

## clang-tidy Integration

clang-tidy runs on open files with the checks the project's `.clang-tidy` enables, and its findings show beside the compiler's.

<!-- BEGIN GENERATED ITEMS: diagnostics/clang_tidy -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy findings**

clang-tidy runs on open files with the checks the nearest `.clang-tidy` enables

Each finding names its check and links to the check's documentation. The checks come from the nearest `.clang-tidy` above the file, as `clang-tidy` itself reads it.

```case
tests/diagnostics/clang_tidy/01_findings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Without a configuration file**

Without a `.clang-tidy`, a small set of checks runs

A project with no `.clang-tidy` still gets a few checks that rarely misfire, such as unused declarations, which an editor shows faded.

```case
tests/diagnostics/clang_tidy/02_default_checks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Silencing with NOLINT**

`NOLINT` comments silence findings on their line, and compiler warnings too

`// NOLINT` silences its own line and `// NOLINTNEXTLINE` the next, for every check or the ones named in parentheses. While clang-tidy runs on the file, they silence compiler warnings on those lines as well.

```case
tests/diagnostics/clang_tidy/04_nolint
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Fixes as quick fixes**

A finding that carries a fix offers it as a quick fix

The fix a check attaches to its finding is offered among the quick fixes at the finding; applying it clears the finding.

```case
tests/diagnostics/clang_tidy/05_quick_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Turning clang-tidy off**

`clang_tidy = false` keeps clang-tidy off open files

With clang-tidy off, open files show only the compiler's diagnostics, whatever the `.clang-tidy` enables.

```case
tests/diagnostics/clang_tidy/09_turned_off
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] Checks too slow for an editor, and those unreliable on code being edited, left out in the editor; `clice lint` runs them
- [x] `.clang-tidy` edits apply to open files at their next compile
- [x] A crash of a clang-tidy check pauses clang-tidy on that file, not its other features
- [x] Suppress clang-tidy warnings originating in system-header macros ([clangd#1587](https://github.com/clangd/clangd/issues/1587), [clangd#2000](https://github.com/clangd/clangd/issues/2000))
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
