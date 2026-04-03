## ADDED Requirements

### Requirement: Header files use a real compilation context
When a header file lacks a direct compile command, `clice` SHALL derive its compilation context from a valid project context rather than falling back to a generic language-default command.

#### Scenario: Opened header borrows project context
- **WHEN** a user opens a header that is included by one or more project translation units but has no direct compile command entry
- **THEN** `clice` selects a valid project-derived compilation context for that header

#### Scenario: No generic fallback for project header
- **WHEN** a project header has at least one valid project-derived compilation context
- **THEN** `clice` does not prefer a generic fallback command over the project-derived context

### Requirement: Header context and compilation context remain distinct runtime concepts
`clice` SHALL distinguish header-context behavior from source-file compilation-context behavior in its runtime model and queries.

#### Scenario: Header query selects header context
- **WHEN** a file is analyzed as a header included through a source file context
- **THEN** `clice` uses header-context selection rules instead of treating the file as an ordinary standalone translation unit

#### Scenario: Source query selects compilation context
- **WHEN** a file is analyzed as a directly compiled source file
- **THEN** `clice` uses compilation-context selection rules appropriate to that source file

### Requirement: Context-sensitive data is queried with an explicit active context
If multiple valid contexts exist for the same path, `clice` SHALL retain enough context identity to select the active one during query execution.

#### Scenario: Multi-context source file remains distinguishable
- **WHEN** the same source file is compiled under multiple valid compile commands
- **THEN** `clice` preserves distinguishable context identities instead of collapsing them into one ambiguous runtime state

#### Scenario: Context-sensitive lookup does not union incompatible states
- **WHEN** a query targets a file whose symbols differ by context
- **THEN** `clice` does not answer by blindly unioning incompatible context-specific results

### Requirement: Missing context is surfaced explicitly
If `clice` cannot determine a valid compilation context for a file, it SHALL surface the degraded state explicitly through diagnostics or logs rather than silently pretending the file has a correct context.

#### Scenario: Missing header context is observable
- **WHEN** `clice` cannot derive a usable context for an opened header
- **THEN** the failure is visible through diagnostics, logs, or both
