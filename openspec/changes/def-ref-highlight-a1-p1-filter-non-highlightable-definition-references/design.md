# Filter non-highlightable definition references

- Canonical Request Title: `fix(highlight/references): filter non-highlightable definition references`
- Conventional Title: `fix(highlight/references)`

## Context

The index layer currently stores a symbol's display name, kind, and referenced files, then serves cross-file relation queries entirely from persisted metadata. That works for ordinary reference lookups, but it is insufficient for clangd-style `canHighlightName` filtering because the decision depends on the original `clang::DeclarationName` kind while AST nodes are no longer available in the query path. The implementation therefore needs a single AST-side predicate and a persisted symbol flag that survives TU builds, project-index merges, on-disk reloads, and open-file indexing.

## Goals / Non-Goals

**Goals:**

- Define one shared declaration-name predicate that matches the requested highlightability policy.
- Persist highlightability in symbol metadata across all index producers and consumers that participate in definition-origin reference highlighting.
- Apply filtering in the query path so only highlight-oriented behavior changes.
- Add negative and positive regression coverage for anonymous, operator-like, identifier, constructor, and destructor cases.

**Non-Goals:**

- Removing relations from the index during AST traversal or merge.
- Changing go-to-definition, workspace-symbol, or general reference behavior outside the approved highlight-oriented path.
- Expanding the scope beyond the requested `canHighlightName` policy.

## Decisions

### 1. Centralize the predicate in `ast_utility`

Add a shared helper such as `ast::can_highlight_name(const clang::DeclarationName&)` in `src/semantic/ast_utility.*`.

Rationale:

- TU indexing and open-file indexing both have AST access, so they can compute the flag once from the real declaration name.
- A shared helper avoids duplicating a fragile `DeclarationName` switch across index and query code.

Alternatives considered:

- Recompute in each caller: rejected because the logic would drift across producers.
- Derive from stored display names: rejected because strings do not reliably distinguish anonymous, operator, selector, and other non-highlightable name kinds.

### 2. Persist `highlightable_name` on `index::Symbol`

Extend `index::Symbol` and the FlatBuffer schema so the flag is written by `TUIndex::serialize`, read by `TUIndex::from`, merged into `ProjectIndex`, and available to both persisted and open-file symbol tables.

Rationale:

- The relation query path runs without live AST nodes, so the decision must be available in symbol metadata.
- Persisting the flag keeps behavior consistent across background indexing, cache reloads, and unsaved-buffer sessions.

Alternatives considered:

- Store the flag only in TU-local data: rejected because project-level queries read merged/persisted symbol tables.
- Filter relations during indexing: rejected because the request explicitly preserves non-highlight flows and only changes highlight-oriented queries.

### 3. Filter only the highlight-oriented definition-origin query path

Gate the filter where indexed references are turned into highlight results for a symbol that resolves from a definition origin, instead of mutating stored relations.

Rationale:

- This isolates the behavior change to the requested feature surface.
- Existing reference relations remain available for other index consumers and future features.

Alternatives considered:

- Remove `Reference` relations for non-highlightable definitions during build: rejected because it would silently change unrelated flows.
- Apply the filter to every relation query: rejected because it is broader than the approved scope.

### 4. Cover both metadata persistence and user-visible behavior in tests

Add unit tests around index query behavior and integration coverage that exercises the persisted index path.

Rationale:

- The bug spans AST analysis, serialized metadata, project-index merge, and query-time filtering.
- Positive tests are needed to prevent accidental regressions for constructors, destructors, and ordinary identifiers.

## Risks / Trade-offs

- Persisted-index schema drift can leave old shards without the new flag -> Mitigation: update the FlatBuffer schema and rely on normal reindexing/rewrite paths so rebuilt indexes always carry the field.
- Open-file and on-disk indexing could diverge if they compute highlightability differently -> Mitigation: route both producers through the same `ast_utility` helper.
- Filtering the wrong query path could change general references unexpectedly -> Mitigation: confine the check to the highlight-oriented definition-origin query and add regression coverage for unaffected flows.

## Migration Plan

- Add the new symbol field and serialization plumbing first so newly built TU and project indexes capture highlightability.
- Rebuild or refresh index data through the existing background-indexing flow; no separate data migration is required.
- Rollback is straightforward because the change is additive to symbol metadata and can be disabled by removing the query-time gate.

## Open Questions

- The implementation should confirm the exact server entry point that backs the owner-requested definition-origin highlight behavior and wire the filter there without broadening it to unrelated reference requests.
