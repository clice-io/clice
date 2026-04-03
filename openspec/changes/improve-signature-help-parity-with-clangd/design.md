## Context

`clice` currently implements signature help in `src/feature/signature_help.cpp` by subclassing `clang::CodeCompleteConsumer`, iterating overload candidates, and manually rendering labels for a handful of candidate kinds. That gives a usable baseline, but it leaves several visible gaps compared with clangd:

- the collector always picks `active_signature = 0` and largely forwards `current_arg` directly, without clangd-style remapping for variadics or other candidate-specific parameter lists
- signature labels are assembled manually rather than from Clang's `CodeCompletionString`, so optional parameters, documentation chunks, and rendered parameter metadata are less faithful
- the server advertises a bare `SignatureHelpOptions{}` rather than clangd-like trigger and retrigger characters
- the request path does not thread signature-help-specific client capabilities or context into feature generation
- tests only verify a minimal overload case and non-crash request flow, while clangd covers overload ordering, function pointers, constructors, aggregates, nested expressions, variadics, templates, stale preambles, and prerequisite modules

clangd's implementation is a good reference because it solves these problems without inventing a separate subsystem: it still uses overload candidates, but builds signatures from `CodeCompletionString`, maps active arguments to rendered parameters, negotiates documentation format and offset support, and validates the behavior with broad regression coverage.

## Goals / Non-Goals

**Goals:**

- Align `clice`'s user-visible signature help behavior with clangd for the major C++ call forms that users hit in practice.
- Advertise and honor signature-help protocol details that affect editor behavior, especially trigger characters, documentation format, and parameter label offsets.
- Make active-parameter selection robust for variadic, defaulted, templated, constructor, aggregate, and function-pointer signatures.
- Add test coverage strong enough to prevent the current behavior gap from reopening.

**Non-Goals:**

- Achieve byte-for-byte parity with every clangd-only extension or internal API.
- Redesign completion, scheduling, or the broader server architecture outside what signature help needs.
- Introduce unrelated LSP features such as snippet completion changes or semantic-token work in this change.

## Decisions

### 1. Keep the overload-candidate pipeline, but switch rendering to `CodeCompletionString`

`clice` should keep using `clang::CodeCompleteConsumer` and `ProcessOverloadCandidates()`, but the collector should stop hand-formatting signatures for each candidate kind wherever Clang already provides a `CodeCompletionString`. This matches clangd's approach and brings several benefits at once:

- rendered labels stay closer to Clang's own completion view
- placeholder and optional chunks can drive parameter extraction directly
- signature and parameter documentation become available without bespoke formatting logic

Alternative considered:

- keep the current manual `switch(candidate.getKind())` rendering and patch missing cases individually. Rejected because it duplicates logic clangd already solved and makes edge cases like optional parameters and documentation much harder to maintain.

### 2. Treat active parameter as a negotiated LSP field backed by candidate-aware remapping

The top-level `SignatureHelp.activeParameter` should remain the primary LSP signal, but `clice` should remap the current argument index against the rendered parameter list for the active candidate instead of forwarding `current_arg` blindly. In particular:

- variadic calls should clamp to the final variadic parameter
- constructor and aggregate initializers should map to the field or constructor slot actually being edited
- function-pointer and template cases should use the rendered parameter list rather than raw AST counts when they differ

Alternative considered:

- add per-signature active-parameter support first. Rejected for now because clangd does not rely on it either, and the highest-value fix is correct top-level `activeParameter` behavior.

### 3. Thread signature-help capabilities and context through the server and worker boundary

The feature layer needs more than just file path and offset. This change should carry enough request metadata to shape the output:

- requested documentation format
- whether parameter label offsets are supported
- any signature-help request context needed for retrigger behavior or follow-up refinements

This keeps protocol negotiation in the server layer while allowing feature code to decide which fields to populate.

Alternative considered:

- hardcode plain-text output and always return offset labels regardless of client support. Rejected because clangd already demonstrates that clients negotiate these details, and ignoring them makes interoperability less predictable.

### 4. Advertise clangd-like trigger coverage, but only promise behavior that `clice` can validate

The server should advertise trigger and retrigger characters for the call and initializer delimiters that matter in C++ editing: `(`, `)`, `{`, `}`, `<`, `>`, and `,`. This is close to clangd and matches the call forms already exercised by clangd's tests.

The implementation should still avoid overpromising beyond validated behavior. If a trigger reaches an unsupported syntactic corner case, the request may still return no signatures, but the advertised surface should cover the common forms users expect in editors.

Alternative considered:

- advertise only `(` and `,`. Rejected because it would keep `clice` visibly behind clangd for constructors, aggregates, and template-argument contexts.

### 5. Use clangd's existing tests as the baseline for `clice` regression coverage

This change should not invent a new test matrix from scratch. Instead, it should translate the highest-value clangd signature-help cases into `clice`'s unit, worker, integration, and module test suites:

- overloads and ordering
- default and optional arguments
- active argument detection inside nested expressions
- constructors, aggregates, and function pointers
- variadics and template arguments
- imported declarations and prerequisite module builds

Alternative considered:

- rely on manual editor checks and the current smoke tests. Rejected because most of the gap is in edge cases that regress silently without precise assertions.

## Risks / Trade-offs

- [Eventide LSP bindings do not expose all signature-help capability fields] -> Extend the local protocol surface or add thin adapter fields in `clice`'s request path, then lock that behavior down with serialization tests.
- [Switching to `CodeCompletionString` changes overload ordering or rendered labels unexpectedly] -> Keep ordering rules explicit and update tests to assert the intended contract rather than incidental string formatting.
- [Documentation lookup adds latency or becomes stale] -> Prefer AST docs when immediately available, use bounded index fallback where it materially improves output, and keep empty-documentation paths cheap.
- [Module and imported-declaration cases depend on prerequisite build state] -> Reuse the existing module test infrastructure and make module-specific coverage part of the acceptance criteria instead of an optional follow-up.

## Migration Plan

1. Extend protocol and options plumbing so signature-help requests can carry client capability details and the server can advertise trigger coverage.
2. Refactor the collector to use richer completion-string data, fix active-parameter mapping, and add documentation support.
3. Add clangd-inspired unit, worker, integration, and module tests until the intended behavior is covered end to end.

Rollback strategy:

- revert to the previous manual rendering path while keeping the safer request plumbing if the richer collector proves unstable
- reduce advertised trigger coverage temporarily if a specific delimiter family produces repeated false positives

## Open Questions

- Whether the current eventide protocol types already carry enough signature-help context, or whether `clice` needs to extend its local wrappers.
- Whether parameter-level documentation is worth populating in this change, or whether signature-level documentation is sufficient for parity.
- Whether `clice` should exactly mirror clangd's trigger set or trim it slightly if one delimiter family remains under-tested after implementation.
