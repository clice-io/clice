## 1. Shared Highlightability Guard

- [ ] 1.1 Add a clangd-equivalent `canHighlightName` helper for `clang::DeclarationName` in the shared semantic AST utilities.
- [ ] 1.2 Apply the helper at the `NamedDecl` occurrence emitters that produce highlightable source ranges without changing relation-only paths.

## 2. Regression Coverage

- [ ] 2.1 Add semantic-token unit tests proving anonymous or empty identifiers plus operator/conversion/literal names are skipped while ordinary identifiers and constructors/destructors remain highlighted.
- [ ] 2.2 Add TUIndex regression tests that confirm skipped names do not appear as highlightable occurrence ranges and unrelated navigation relations still remain queryable.

## 3. Validation

- [ ] 3.1 Run targeted semantic-token and TUIndex unit coverage with the existing unit-test workflow, such as `pixi run unit-test Debug`.
- [ ] 3.2 Run the broader `pixi run test Debug` suite if the targeted tests pass and the full run is feasible in the lane.
