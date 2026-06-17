# Code Action

clice advertises `textDocument/codeAction` support but currently returns an empty list. This page tracks the intended scope.

## Quick Fixes

Actions derived from `FixItHint`s attached to clang / clang-tidy diagnostics.

- [ ] Apply a compiler `FixItHint` as a quick fix
- [ ] Apply clang-tidy fix-its
- [ ] `source.fixAll` — batch-apply all available fixes in the file ([clangd#1446](https://github.com/clangd/clangd/issues/1446))
- [ ] "Fix all occurrences" of the same diagnostic kind in one action ([clangd#830](https://github.com/clangd/clangd/issues/830))
- [ ] Apply fixes whose edits fall outside the main file ([clangd#1747](https://github.com/clangd/clangd/issues/1747))
- [ ] Honor client code-action capabilities (`isPreferred`, resolve support) ([clangd#573](https://github.com/clangd/clangd/issues/573))
- [ ] Optionally apply formatting to code-action edits ([clangd#2476](https://github.com/clangd/clangd/issues/2476))

## Include Actions

- [ ] Add a missing `#include` for an unresolved symbol ([clangd#1017](https://github.com/clangd/clangd/issues/1017))
- [ ] Insert includes using project-relative paths, not absolute ([clangd#2010](https://github.com/clangd/clangd/issues/2010))
- [ ] Configurable include style — quoted vs. angle brackets ([clangd#1367](https://github.com/clangd/clangd/issues/1367))
- [ ] Remove unused `#include` (include-cleaner)
- [ ] Respect IWYU pragmas (`export`, `keep`, `private`)
- [ ] Suggest a `using` declaration as alternative to qualifying an unresolved symbol ([clangd#976](https://github.com/clangd/clangd/issues/976))
- [ ] C files: suggest `<stdlib.h>` not `<cstdlib>` ([clangd#2246](https://github.com/clangd/clangd/issues/2246))

## Refactorings

Cursor/selection-driven refactorings.

### Extract

- [ ] Extract variable ([clangd#446](https://github.com/clangd/clangd/issues/446))
- [ ] Extract variable should replace all occurrences of the expression ([clangd#924](https://github.com/clangd/clangd/issues/924))
- [ ] Extract variable in macro arguments ([clangd#1197](https://github.com/clangd/clangd/issues/1197))
- [ ] Extract function / method ([clangd#698](https://github.com/clangd/clangd/issues/698))
- [ ] Extract function should preserve placeholder return types (`auto`) ([clangd#653](https://github.com/clangd/clangd/issues/653))
- [ ] Extract function must not introduce desugared types ([clangd#1496](https://github.com/clangd/clangd/issues/1496))
- [ ] Extract function must handle types defined in enclosing scope ([clangd#1710](https://github.com/clangd/clangd/issues/1710))
- [ ] Extract function for C files ([clangd#1810](https://github.com/clangd/clangd/issues/1810))

### Inline / Expand

- [ ] Inline variable / function
- [ ] Expand `auto` / deduced type
- [ ] Expand macro one level ([clangd#820](https://github.com/clangd/clangd/issues/820))

### Move / Define

- [ ] Define method out-of-line (move body out of the class)
- [ ] Define method inline (move body into the declaration)
- [ ] Generate a missing method definition from declaration ([clangd#445](https://github.com/clangd/clangd/issues/445))
- [ ] Generate a missing declaration from out-of-line definition ([clangd#2454](https://github.com/clangd/clangd/issues/2454), [clangd#730](https://github.com/clangd/clangd/issues/730))

### Transform

- [ ] Add a `using` declaration ([clangd#73](https://github.com/clangd/clangd/issues/73))
- [ ] Replace `using namespace` with explicit `using` declarations ([clangd#1067](https://github.com/clangd/clangd/issues/1067))
- [ ] Drop qualifiers or replace with a type alias ([clangd#1619](https://github.com/clangd/clangd/issues/1619))
- [ ] Swap / invert `if` branches ([clangd#466](https://github.com/clangd/clangd/issues/466))
- [ ] Populate `switch` cases ([clangd#807](https://github.com/clangd/clangd/issues/807))
- [ ] Convert to / from a raw string literal
- [ ] Create a declaration from a usage ([clangd#467](https://github.com/clangd/clangd/issues/467))
- [ ] Remove function / method ([clangd#2580](https://github.com/clangd/clangd/issues/2580))
- [ ] Modify function parameters and update call sites ([clangd#460](https://github.com/clangd/clangd/issues/460))
- [ ] Fix mismatched declaration/definition signatures ([clangd#77](https://github.com/clangd/clangd/issues/77))
- [ ] Implement pure virtual methods of base class ([clangd#1037](https://github.com/clangd/clangd/issues/1037))

## Changelog

| Date | Change | PR |
| ---- | ------ | -- |
| —    | Stub handler (always returns empty list) | — |
