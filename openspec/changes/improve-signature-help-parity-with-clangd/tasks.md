## 1. Protocol And Request Plumbing

- [ ] 1.1 Extend signature-help capability advertisement and request plumbing to carry trigger or retrigger characters, documentation format, parameter label-offset support, and any required request context across the server and worker boundary.
- [ ] 1.2 Expand `SignatureHelpOptions` and related protocol serialization tests so feature code can choose output shape based on negotiated client capabilities.

## 2. Signature Collection Parity

- [ ] 2.1 Refactor `src/feature/signature_help.cpp` to build labels and parameters from richer completion-string data instead of relying only on manual overload formatting.
- [ ] 2.2 Add candidate-aware active-parameter mapping for variadic, defaulted, templated, constructor, aggregate, and function-pointer signatures, keeping overload ordering deterministic.
- [ ] 2.3 Populate signature documentation and parameter metadata from AST comments and index or preamble lookups where available, without regressing empty-documentation cases.

## 3. Edge Cases And Module Coverage

- [ ] 3.1 Verify signature help for nested calls, opening delimiters, braced initializers, function pointers, and imported or module-provided declarations, fixing any parser or snapshot assumptions that block these cases.
- [ ] 3.2 Add or update worker and server handling needed for stale compile state and prerequisite module builds so signature help remains available after document edits and module preparation.

## 4. Validation

- [ ] 4.1 Expand `tests/unit/feature/signature_help_tests.cpp` with clangd-inspired cases for overloads, active arguments, default arguments, variadics, templates, constructors, aggregates, and nested expressions.
- [ ] 4.2 Add server and worker tests that assert capability advertisement and returned signature metadata instead of only checking for non-crash behavior.
- [ ] 4.3 Run the relevant signature-help, stateless-worker, integration, and module test targets, then fix any regressions uncovered during verification.
