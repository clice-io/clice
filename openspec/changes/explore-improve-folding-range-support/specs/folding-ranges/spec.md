## ADDED Requirements

### Requirement: Folding range responses honor client capabilities
The server SHALL render folding ranges according to the client's declared folding capabilities instead of always returning the richest possible payload.

#### Scenario: Line-only folding is respected
- **WHEN** the client declares `textDocument.foldingRange.lineFoldingOnly = true`
- **THEN** the server MUST return folding ranges that remain valid when interpreted as whole-line folds, including adjusting end boundaries for bracketed or comment ranges whose closing delimiter is on the last line

#### Scenario: Client line-only support is propagated through folding options
- **WHEN** the client declares `textDocument.foldingRange.lineFoldingOnly = true`
- **THEN** the server MUST invoke folding rendering with options equivalent to `line_folding_only = true`
- **AND** collectors MUST NOT need to inspect client capability state to produce different raw ranges

#### Scenario: Collapsed text is gated by client support
- **WHEN** the client does not declare support for `textDocument.foldingRange.foldingRange.collapsedText`
- **THEN** the server MUST omit `collapsedText` from the folding range response

#### Scenario: Preferred range limits are applied deterministically
- **WHEN** the client declares `textDocument.foldingRange.rangeLimit = N` and the server can produce more than `N` folding ranges for a document
- **THEN** the server MUST return no more than `N` ranges and MUST choose them using a deterministic ordering rule

#### Scenario: Standard kinds are emitted compatibly
- **WHEN** a folding range represents a comment block, an include/import block, or any other foldable region
- **THEN** the server MUST emit `kind = comment`, `kind = imports`, or `kind = region` respectively, and MUST NOT require clients to understand clice-specific kind strings in order to fold correctly

### Requirement: Structural and comment folding baseline
The server SHALL provide folding ranges for multi-line C/C++ structural regions and multi-line comments in the main file.

#### Scenario: Multi-line comment blocks can be folded
- **WHEN** a document contains a multi-line `/* ... */` comment or a contiguous block of `//` comments spanning more than one line
- **THEN** the server MUST return a folding range for that comment block with `kind = comment`

#### Scenario: Single-line comments are not folded
- **WHEN** a document contains a single-line comment that does not extend across multiple lines and is not part of a larger contiguous comment block
- **THEN** the server MUST NOT return a folding range for that comment

#### Scenario: Existing structural regions remain foldable
- **WHEN** a document contains a multi-line namespace, record, function body, parameter list, lambda body, initializer list, or other supported structural region already collected by clice
- **THEN** the server MUST continue to return a folding range for that region if its boundaries can be mapped back to the main file

### Requirement: Preprocessor regions fold as complete branch blocks
The server SHALL provide complete and nested folding ranges for preprocessor branch structures instead of leaving the final branch in a conditional block unclosed.

#### Scenario: Final conditional branch closes at endif
- **WHEN** a document contains a `#if/#elif/#else/#endif` chain
- **THEN** the server MUST generate a folding range for each multi-line branch body, including the last branch body that ends at `#endif`

#### Scenario: Inactive conditional branches can be folded
- **WHEN** a conditional branch is known to be inactive or skipped in the current preprocessing configuration
- **THEN** the server MUST be able to return a folding range covering that inactive branch region using `kind = region`

#### Scenario: Nested pragma regions are folded
- **WHEN** a document contains nested `#pragma region` / `#pragma endregion` pairs in the main file
- **THEN** the server MUST return properly nested folding ranges for each matched region pair

### Requirement: C/C++ directive groups and multiline macros are foldable
The server SHALL use clice's preprocessor metadata to expose foldable ranges that clangd does not currently provide.

#### Scenario: Multi-line macro definitions can be folded
- **WHEN** a document contains a multi-line macro definition whose body spans more than one physical line
- **THEN** the server MUST return a folding range for that macro definition using `kind = region`

#### Scenario: Consecutive include directives are grouped
- **WHEN** a document contains a contiguous block of `#include` directives with no intervening non-trivia code lines
- **THEN** the server MUST return a folding range covering that include block using `kind = imports`

#### Scenario: Consecutive module imports are grouped
- **WHEN** a document contains a contiguous block of C++ module `import` declarations with no intervening non-trivia code lines
- **THEN** the server MUST return a folding range covering that import block using `kind = imports`
