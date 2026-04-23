## ADDED Requirements

### Requirement: Folding ranges are normalized before response emission
The server SHALL convert collected folding candidates into a deterministic normalized set before emitting the folding range response.

#### Scenario: Duplicate candidates collapse to one emitted fold
- **WHEN** multiple collectors produce the same folding candidate for the same source span and raw metadata
- **THEN** the server MUST emit at most one folding range for that candidate

#### Scenario: Invalid candidates are dropped during normalization
- **WHEN** a collected folding candidate does not span multiple lines or cannot be mapped back to the main file
- **THEN** the server MUST omit that candidate from the emitted folding ranges

#### Scenario: Output ordering is deterministic
- **WHEN** the same document is analyzed repeatedly without source changes
- **THEN** the server MUST emit folding ranges in a deterministic order that does not depend on collector traversal order

### Requirement: Existing structural folding survives the pipeline split
The server SHALL preserve the currently supported AST structural folding categories after collection, normalization, and rendering are separated.

#### Scenario: Supported structural regions remain foldable
- **WHEN** a document contains a supported multi-line namespace, record, function body, parameter list, lambda body, initializer list, call argument list, or compound statement
- **THEN** the server MUST still return a folding range for that region when its boundaries can be mapped to the main file

#### Scenario: Structural coverage is preserved through normalization
- **WHEN** the document contains only currently supported AST-driven folding categories
- **THEN** normalization and rendering MUST NOT remove a valid structural fold except when it is an exact duplicate or an invalid range

### Requirement: Rendering decisions are applied after normalization
The server SHALL derive final LSP folding-range output from normalized internal ranges instead of requiring collectors to emit protocol-shaped results directly.

#### Scenario: Rendering options do not require collector changes
- **WHEN** rendering rules change how line or metadata output is shaped for a normalized fold
- **THEN** the server MUST apply that change in the rendering phase without requiring collector-specific logic changes

#### Scenario: Metadata hints remain optional until rendering
- **WHEN** a collected or normalized fold carries optional kind or collapsed-text hints
- **THEN** the renderer MUST decide whether to surface, transform, or suppress that metadata in the emitted LSP range

### Requirement: Folding rendering is configured through explicit options
The server SHALL expose folding-specific rendering options so client capability behavior can be selected without changing collectors or raw ranges.

#### Scenario: Default options preserve existing output
- **WHEN** folding ranges are requested without explicit folding options
- **THEN** rendering MUST behave as if `line_folding_only = false`

#### Scenario: Line-only rendering is selected by options
- **WHEN** folding ranges are rendered with `line_folding_only = true`
- **THEN** the renderer MUST emit ranges that remain valid when interpreted as whole-line folds
- **AND** collectors MUST NOT need to inspect client capability state or emit different raw ranges for line-only clients
