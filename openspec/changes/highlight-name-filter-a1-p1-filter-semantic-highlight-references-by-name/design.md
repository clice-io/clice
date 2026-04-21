## Request Metadata

- Proposal Title: Filter semantic highlight references by name eligibility
- Canonical Request Title: `fix(semantic-tokens): filter ineligible highlight references`
- Conventional Title: `fix(semantic-tokens)`

## Context

The repository's current highlight-producing path is semantic-token generation in `src/feature/semantic_tokens.cpp`; it does not expose a dedicated `textDocument/documentHighlight` implementation. In that collector, `handleDeclOccurrence()` emits tokens for all referenced `NamedDecl`s, while clangd applies a `canHighlightName` gate so unsupported declaration-name kinds are skipped before reference highlights are emitted.

The requested behavior maps cleanly to clice's semantic-token pipeline. The change needs to stay narrow: reuse clangd's name-eligibility semantics, keep the filtering logic easy to share, and protect constructor/destructor highlighting, which clangd explicitly treats as eligible.

## Goals / Non-Goals

**Goals:**
- Mirror clangd's declaration-name eligibility rules in a reusable helper at the AST utility layer.
- Apply that helper to semantic-token reference occurrences so unsupported names no longer produce reference tokens.
- Preserve constructor and destructor reference highlighting with focused regression tests.
- Keep the proposal scoped to semantic-token highlighting so implementation can proceed without index or protocol churn.

**Non-Goals:**
- Changing `textDocument/references` or introducing `textDocument/documentHighlight`
- Updating index serialization, symbol naming, or workspace-symbol behavior
- Reworking declaration or definition token handling unless the concrete failing repro shows those paths are also incorrect

## Decisions

### 1. Add a reusable declaration-name eligibility helper in `src/semantic/ast_utility.{h,cpp}`

The helper should accept `clang::DeclarationName` and implement the same allow/deny split as clangd's `canHighlightName`: allow non-empty identifiers plus constructor/destructor names; reject Objective-C selectors, conversion functions, overloaded operators, deduction guides, literal operators, and using directives.

This location keeps the rule close to other declaration-name helpers such as `name_of()` and makes it available to future semantic features without duplicating the switch logic.

Alternatives considered:
- Inline the switch in `src/feature/semantic_tokens.cpp`: rejected because it would hard-code clangd-mirroring logic inside one feature file and make reuse harder.
- Derive eligibility from rendered strings such as `name_of()`: rejected because string formatting is not the same contract as source-highlightability and would blur the intended rule.

### 2. Gate only reference semantic-token emission with the helper

`SemanticTokensCollector::handleDeclOccurrence()` already knows the occurrence relation. The new helper should be consulted only for reference-style relations before token emission, leaving existing declaration and definition paths unchanged for this proposal.

This matches the request, reduces regression risk, and keeps the change aligned with clangd's reference-highlight guard rather than broadening it into a generic token filter.

Alternatives considered:
- Apply the helper to every occurrence kind: rejected because the request is specifically about reference highlighting and broader filtering could hide valid declaration/definition tokens.
- Filter earlier in the semantic visitor or index layer: rejected because the problem is local to semantic-token emission and those layers feed other behaviors that are explicitly out of scope.

### 3. Add focused semantic-token regression coverage instead of a broad name-kind matrix

Tests should cover one real unsupported-name reference repro and one allowed constructor/destructor path. This keeps the proposal targeted while protecting the two most important outcomes: suppression of ineligible references and preservation of eligible special member references.

Alternatives considered:
- Exhaustively test every `DeclarationName` kind in this change: rejected because it adds review and maintenance cost beyond the scope needed for approval.
- Rely on manual verification: rejected because the change is easy to regress and semantic-token behavior already has unit coverage to extend.

## Risks / Trade-offs

- Divergence from clangd if its eligibility rules change later -> Centralize the switch in one helper and keep it structurally close to clangd's implementation so updates are straightforward.
- Some unsupported names may already be skipped earlier because they lack stable source locations -> Base the regression on a currently emitted failing repro and keep extra scenarios limited to cases the collector can actually observe.
- Constructor/destructor handling could regress while adding the filter -> Add an explicit positive test for constructor/destructor references in the same unit test file.

## Migration Plan

No data or protocol migration is required. Land the helper, the semantic-token collector change, and the regression tests together so the behavior shift is atomic and easily reversible by reverting the code change.

## Open Questions

No open product questions remain for planning. During implementation, the only technical check is selecting the unsupported-name repro that the current semantic-token collector actually emits today.
