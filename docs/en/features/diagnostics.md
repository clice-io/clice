# Diagnostics

Implementation: `src/feature/diagnostics.cpp`, `src/server/compiler/compiler.cpp`

## Core

- [x] Clang diagnostics (errors, warnings, notes)
- [x] Severity mapping (Error, Warning)
- [x] Diagnostic ranges with source locations
- [x] Related information (notes attached to diagnostics)
- [x] File URI conversion for cross-file diagnostics

## Tags

- [x] `Deprecated` tag for `-Wdeprecated` diagnostics
- [x] `Unnecessary` tag for unused variable/parameter warnings

## Publishing

- [x] Push diagnostics on compilation completion
- [x] Clear diagnostics on file close
- [x] Per-file diagnostic grouping (interested file + headers)

## Optional Integrations

- [ ] clang-tidy diagnostics (gated by `clang_tidy` config) [^clice-90]
- [ ] Code action quickfixes from `FixItHint`
- [ ] Diagnostic suppression comments (`// NOLINT`)

[^clice-90]: [clice#90](https://github.com/clice-project/clice/issues/90) — clang-tidy integration tracking issue `open`
