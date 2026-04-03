## ADDED Requirements

### Requirement: Opened buffer content is authoritative
When a document is opened in the editor, `clice` SHALL treat the opened buffer content as the source of truth for compile inputs and AST-backed queries until the document is closed.

#### Scenario: Hover uses unsaved content
- **WHEN** a user edits an opened file without saving and requests hover in that file
- **THEN** `clice` uses the opened buffer content rather than the on-disk file content to answer the request

#### Scenario: Completion uses unsaved content
- **WHEN** a user edits an opened file without saving and requests completion in that file
- **THEN** `clice` builds completion context from the opened buffer content rather than the on-disk file content

### Requirement: AST-backed requests use a matching document generation
`clice` SHALL answer AST-backed requests only from an AST generation that matches the current opened-document version, or otherwise reject/defer the request in a defined way.

#### Scenario: Stale AST result is discarded
- **WHEN** a rebuild completes for an older document version after a newer edit has already been accepted
- **THEN** `clice` discards the stale AST result and does not publish it as the current answer state

#### Scenario: Feature request does not read stale AST
- **WHEN** a feature request arrives while the current document version has not yet produced a matching AST
- **THEN** `clice` does not answer from an older AST generation

### Requirement: Active-file navigation starts from live document state
For an opened file, `clice` SHALL resolve the source-side symbol and position mapping for navigation requests from the current document state before using disk-backed index data for remote results.

#### Scenario: Local definition lookup uses live offsets
- **WHEN** a user makes unsaved edits that shift symbol offsets and requests definition from the opened file
- **THEN** `clice` resolves the queried symbol from the current opened document state instead of interpreting the request against stale disk offsets

### Requirement: Closing a document ends opened-buffer authority
When an opened document is closed, `clice` SHALL stop treating the editor buffer as authoritative and SHALL release associated opened-document runtime state.

#### Scenario: Closed document falls back to disk state
- **WHEN** a document is closed and then queried again without reopening
- **THEN** `clice` uses persisted project state rather than the former unsaved editor buffer

#### Scenario: Close releases owned runtime state
- **WHEN** a document is closed
- **THEN** `clice` releases worker ownership and opened-document runtime state associated with that document
