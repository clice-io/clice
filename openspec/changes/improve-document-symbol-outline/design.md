## Context

The current `src/feature/document_symbols.cpp` implementation reuses the generic `FilteredASTVisitor`. Its advantage is brevity, but it does not model outline-specific semantics such as which declarations should become outline nodes, which should only forward traversal to children, and which subtrees should be skipped entirely.

Compared with `clangd`'s `DocumentOutline`, the current implementation has several structural shortcomings:

- Function nodes continue recursive traversal into function bodies, so local `VarDecl`, `RecordDecl`, `EnumDecl`, `BindingDecl`, and similar declarations may leak into the document outline.
- Only implicit template instantiations are filtered today; explicit instantiations and explicit specializations do not have distinct visit policies.
- Declarations produced by macro expansion are placed directly according to the expanded declaration instead of being grouped under main-file macro invocations as `clangd` does.
- The implementation computes `range` / `selection_range` per declaration but does not repair parent-child range relationships after tree construction, and it does not provide fallback handling for macro locations or inconsistent written-name ranges.
- Existing tests mostly assert total node counts, which is too weak to prevent hierarchy, naming, and range regressions.

## Goals / Non-Goals

**Goals:**

- Align `clice` document outline behavior with `clangd` on the core semantic cases.
- Show only document-level symbols users actually care about while preserving namespace, type, enum, and member hierarchy.
- Define stable, testable behavior for templates and macros.
- Replace count-only assertions with structural assertions that provide long-term regression protection.

**Non-Goals:**

- This change does not introduce `#pragma mark`-style outline grouping.
- This change does not modify `workspace/symbol` behavior or index-layer symbol handling.
- This change does not refactor unrelated AST traversal infrastructure.

## Decisions

### 1. Use a dedicated outline traversal instead of piling more special cases onto `FilteredASTVisitor`

The core issue in document symbol collection is not "filter a few more decl kinds". It is the need to explicitly distinguish:

- do not visit
- visit declaration only
- visit children only
- visit both declaration and children

This is exactly what `clangd`'s `VisitKind` model addresses. Keeping the generic visitor and layering more contextual flags on top would scatter function-body filtering, template behavior, and macro-container insertion across multiple callbacks and make the implementation harder to maintain.

The alternative is to keep the current visitor and add more contextual checks such as "do not collect local declarations inside function bodies". That approach is smaller, but it does not naturally express leaf-only explicit instantiations, wrapper decl pass-through, or macro container insertion, so it is not chosen.

### 2. Define template and wrapper-declaration behavior using explicit visit policies

The new traversal should cover at least these rules:

- `LinkageSpecDecl`, `ExportDecl`, and similar wrappers do not become outline nodes and only forward traversal to their children.
- Functions and methods become nodes, but traversal must not descend into function-local declarations.
- Implicit template instantiations are skipped entirely.
- Explicit instantiations are shown as declaration-only leaf nodes and do not synthesize member children.
- Explicit specializations show both the declaration itself and the children actually written in source.

This makes the outline reflect the structure users wrote in the file instead of all declarations that happen to be traversable in the AST.

### 3. Create container nodes for macro invocations written in the main file

`clangd` includes macro invocations written directly in the main file as outline hierarchy nodes. `clice` already has enough macro-location decomposition support to adopt a similar strategy:

- When a declaration originates from macro expansion, walk up the macro-caller chain until reaching a direct invocation written in the main file.
- Create at most one container node per invocation site.
- Attach all outline declarations produced by that invocation under the macro container.

This avoids having multiple macro-expanded declarations appear as if they were written directly in the enclosing namespace or class scope, and it keeps the outline aligned with the entry points users actually see in the source.

The alternative is to keep the current behavior and only repair ranges. That is simpler to implement, but it leaves macro-heavy outlines difficult to understand, so it is not chosen.

### 4. Repair ordering and ranges after building the tree

The protocol requires `selectionRange` to be contained within `range`, and editors generally assume parent ranges contain child ranges. The current implementation sorts declarations individually but does not apply a tree-level repair pass.

This change adds a post-processing step that:

- sorts siblings stably in source order
- expands parent ranges when child ranges extend beyond the initially collected parent range
- repairs inconsistent `selectionRange` values by falling back to a more reliable location, and collapses to a single range if necessary

This keeps editor-facing invariants centralized instead of scattering boundary-fix logic across declaration collection paths.

### 5. Test structure directly instead of asserting only node counts

New and updated tests should assert:

- top-level and child hierarchy
- `name` / `kind` for key nodes
- the absence of leaked local declarations
- macro container presence and correct children
- expected behavior for explicit instantiations and explicit specializations
- `selectionRange` containment within `range`

This is the minimum needed to cover the semantics that actually matter in this change.

## Risks / Trade-offs

- [Higher implementation complexity] -> The traversal logic will be longer than the current visitor-based version. Split visit-policy handling, macro-container logic, and range repair into separate helpers to keep it manageable.
- [Macro container icon semantics may be imperfect] -> LSP `SymbolKind` does not offer an ideal macro-specific visual category. Prioritize correct hierarchy and ranges first.
- [Template edge cases are numerous] -> Cover the highest-value cases first with tests for implicit instantiation, explicit instantiation, and explicit specialization behavior.
- [Snapshot-style tests would be brittle] -> Prefer structural assertions over full JSON snapshots to reduce churn from unrelated field changes.

## Open Questions

- Should this change also add deprecated document symbol metadata, depending on whether the current protocol schema supports `DocumentSymbol.tags` or `deprecated` fields cleanly?
