## Context

`clice` already exposes `textDocument/semanticTokens/full` and advertises a legend derived from `SymbolKind` and `SymbolModifiers`, but the implementation in `src/feature/semantic_tokens.cpp` only emits:

- lexical tokens such as comments, strings, numbers, directives, headers, and keywords
- declaration/definition markers for named declarations and macros
- a template marker for templated declarations

This leaves most of the declared semantic surface unused. In particular, `SymbolKind::Module`, `Attribute`, `Operator`, `Paren`, `Bracket`, `Brace`, `Angle`, `Concept`, and several declaration kinds/modifiers are either never emitted or emitted only indirectly. The current merge logic also degrades overlapping classifications to `Conflict`, which throws away useful semantic information instead of preferring the more specific token.

The gap is visible when compared with clangd: clangd classifies more AST constructs, supports semantic token delta responses with `resultId`, and refreshes tokens when a more accurate AST becomes available. `clice` also already contains C++20 module infrastructure, `directive.imports`, `CompilationUnitRef::module_name()`, and a reserved `handleModuleOccurrence()` hook, so module-aware semantic tokens fit the current architecture rather than requiring a new subsystem.

## Goals / Non-Goals

**Goals:**

- Emit materially richer semantic token kinds and modifiers from AST information instead of relying mostly on lexical fallback.
- Highlight C++20 module declarations and imports, including named modules and partitions.
- Replace `Conflict`-style overlap handling with deterministic precedence so semantic meaning is preserved.
- Support semantic token `resultId` and `/full/delta` responses, with cache invalidation and refresh when rebuilds change highlighting.
- Add tests that lock in token kinds, modifiers, and module-specific behavior.

**Non-Goals:**

- Achieve byte-for-byte parity with clangd's full highlighting matrix in a single change.
- Add `textDocument/semanticTokens/range`.
- Add end-user configuration for disabling token kinds/modifiers in this change.
- Redesign the broader `SymbolKind` taxonomy used by completion, hover, or indexing.

## Decisions

### 1. Keep the existing two-phase model, but turn semantic tokens into an overlay with precedence

The collector will continue to start with a lexical pass so comments, literals, directives, and headers remain available even before AST-specific enrichment. Semantic passes will then add higher-priority tokens for declarations, operators, attributes, concepts, brackets, and modules.

Instead of collapsing equal-range overlaps to `Conflict`, the merge step will apply explicit precedence:

- semantic classification beats lexical classification
- more specific semantic kinds beat generic ones
- modifiers are merged when the underlying symbol identity is compatible
- `Conflict` remains only as a last-resort debugging state, not the normal output

Why this approach:

- it preserves the current fast lexical baseline
- it minimizes churn in call sites
- it matches how editors expect semantic tokens to augment syntax coloring rather than erase it

Alternative considered:

- emit semantic tokens from AST only and drop lexical fallback entirely. Rejected because it would lose highlighting for comments/directives/headers and make partially-built AST states visibly worse.

### 2. Use existing compile-layer module metadata for module token extraction

Module highlighting will use data already available in `CompilationUnitRef` and compile directives:

- `CompilationUnitRef::module_name()` and `is_module_interface_unit()` for the current unit
- `directives().imports` for imported module names
- `TokenBuffer` / spelled token APIs for mapping module names and separators to source ranges

This is preferred over trying to rely only on `VisitImportDecl` because the repository already captures import metadata during preprocessing, and the current `SemanticVisitor` module hooks are intentionally incomplete. The design can still route final token creation through `handleModuleOccurrence()`, but the source of truth for locating module-name tokens should be the compile-layer token data.

Alternative considered:

- finish the commented-out `SemanticVisitor::VisitImportDecl` path first and derive all module tokens from AST traversal. Rejected for now because preprocessing already records import structure reliably, while AST-only extraction is more fragile around partitions and special module forms.

### 3. Fill the most valuable semantic gaps first

The implementation should prioritize the categories that are already modeled in `SymbolKind`/`SymbolModifiers` and easy to validate in tests:

- declaration kinds already discoverable from `NamedDecl`
- module names and partitions
- attributes such as `final` / `override` and explicit attribute syntax
- overloaded and built-in operators
- bracket-like punctuation tied to templates and explicit operators/casts
- modifiers such as `Const`, `Overloaded`, and `Typed` where the current AST plumbing can support them without ambiguity

Why this scope:

- it closes the largest visible gap quickly
- it avoids inventing new token kinds before the existing taxonomy is exercised
- it keeps the change aligned with the user's clangd comparison request

Alternative considered:

- introduce new token kinds/modifiers to mirror clangd more directly. Rejected because `clice` already has an internal taxonomy and the first priority is to use it consistently.

### 4. Add delta responses in the server layer, not inside feature collection

`feature::semantic_tokens()` should continue returning a full logical token stream for one snapshot of a document. Delta computation, `resultId` assignment, and cache management belong in the server/request layer because they depend on per-document history rather than AST semantics.

Expected server changes:

- advertise `semanticTokensProvider.full.delta = true`
- accept `textDocument/semanticTokens/full/delta`
- cache the last encoded token stream per document
- invalidate the cache on close, rebuild, and version changes that replace the document snapshot
- send `workspace/semanticTokens/refresh` when the server has moved from a stale/partial semantic view to a fresh compiled view and the client supports refresh

Alternative considered:

- compute deltas in the feature layer on raw tokens. Rejected because delta state must be keyed by document lifecycle and LSP request history, which the feature layer does not own.

### 5. Treat tests as the contract for token stability

This change will expand test coverage instead of relying on manual editor inspection alone:

- unit tests for token kinds/modifiers in small annotated snippets
- unit tests for merge precedence and multiline encoding invariants
- integration tests for full-token and delta requests
- module fixture tests covering named modules, partitions, global module fragments, private fragments, and importers

Alternative considered:

- validate only through smoke tests in an editor. Rejected because token regressions are subtle and easy to reintroduce without precise assertions.

## Risks / Trade-offs

- [Token overlap rules become too ad hoc] → Define a small explicit precedence table and test it directly instead of scattering special cases through the merge code.
- [Template angle brackets and parser-token splitting differ from `TokenBuffer` behavior] → Scope bracket highlighting to ranges that can be proven from spelled tokens and add regression tests for nested templates.
- [Module syntax has corner cases such as global module fragments, private fragments, and partitions] → Require stable highlighting only for named module/import identifiers and avoid inventing semantics for punctuation that cannot be located robustly.
- [Delta cache becomes stale after recompilation or close/reopen cycles] → Tie cache lifetime to document URI/version and clear it on close, invalidation, and failed rebuilds.
- [More semantic passes increase feature latency] → Keep lexical scanning unchanged, reuse existing AST traversal hooks, and benchmark only if tests show material slowdown.

## Migration Plan

1. Extend token collection and merge rules while preserving the current full-token API.
2. Add module-aware token extraction and expand unit tests.
3. Wire server-side delta support and refresh/invalidation behavior.
4. Update integration tests to cover both full and delta semantic token requests.

Rollback strategy:

- disable delta advertisement and continue serving full tokens if cache invalidation proves unstable
- keep lexical fallback so the editor still receives usable highlighting even if some semantic categories are temporarily rolled back

## Open Questions

- Whether `Const`, `Typed`, and `Overloaded` should be emitted only when derived with high confidence, or whether some heuristic cases are acceptable.
- Whether refresh notifications should be sent only after successful recompilation or also after dependency-driven module invalidation.
- Whether bracket/operator highlighting should be limited to the kinds already modeled in `SymbolKind`, or whether a follow-up change should align names more closely with standard LSP token vocabularies.
