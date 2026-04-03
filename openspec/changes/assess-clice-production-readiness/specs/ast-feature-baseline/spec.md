## ADDED Requirements

### Requirement: Production rollout includes a minimum AST-only feature baseline
Before internal production dogfooding, `clice` SHALL provide a stable AST-only feature baseline consisting of hover, completion, signature help, document symbols, document links, folding ranges, semantic tokens, inlay hints, formatting, diagnostics, document highlight, selection range, and AST-backed code actions that do not require a global index.

#### Scenario: Missing AST-only feature is visible in capability surface
- **WHEN** an AST-only feature is not implemented to the agreed baseline
- **THEN** `clice` does not advertise it as available

#### Scenario: Implemented AST-only feature is routed end-to-end
- **WHEN** an AST-only feature reaches the agreed baseline
- **THEN** `clice` exposes it through LSP capability advertisement, request routing, and worker execution

### Requirement: Hover provides semantic detail beyond symbol name
Hover responses SHALL include semantic information that is useful for real editing, such as declaration spelling, type information, or associated documentation when available.

#### Scenario: Hover on function shows semantic information
- **WHEN** a user hovers a function or method symbol
- **THEN** the hover response includes more than a bare symbol kind and name

### Requirement: Completion and signature help expose actionable assistance
Completion and signature-help responses SHALL provide parameter-aware assistance that is suitable for day-to-day editing, including snippets or argument guidance when enabled by server options.

#### Scenario: Completion inserts parameter-aware text
- **WHEN** snippet-style completion support is enabled and a callable symbol is completed
- **THEN** the completion response includes parameter-aware insertion behavior

#### Scenario: Signature help identifies active parameter
- **WHEN** a user requests signature help inside a callable argument list
- **THEN** the response identifies the active parameter for the current cursor position

### Requirement: Inlay hint computation respects requested scope
Inlay hints SHALL be computed for the requested document scope rather than always forcing a full-document computation.

#### Scenario: Range request limits inlay hint computation
- **WHEN** a client requests inlay hints for a document range
- **THEN** `clice` computes and returns hints for that range instead of scanning the entire file

### Requirement: AST-only code actions are either implemented or hidden
If an AST-only code action is advertised by `clice`, it SHALL produce a meaningful result instead of an unconditional empty placeholder response.

#### Scenario: Advertised code action is not a stub
- **WHEN** a client requests code actions for a context where an advertised AST-only code action applies
- **THEN** `clice` returns a meaningful action or omits the capability from advertisement
