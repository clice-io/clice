## ADDED Requirements

### Requirement: Worker lifecycle changes are reflected in runtime ownership
`clice` SHALL keep worker ownership, opened-document tracking, and eviction state consistent across open, change, save, close, and worker-failure events.

#### Scenario: Closing a document releases worker ownership
- **WHEN** a document is closed
- **THEN** `clice` removes the document from worker ownership tracking and allows its runtime state to be evicted

#### Scenario: Worker failure is surfaced
- **WHEN** a worker crashes, times out, or fails IPC
- **THEN** `clice` records the failure and surfaces a defined error outcome instead of silently pretending the request succeeded

### Requirement: Runtime resource controls are enforced rather than decorative
Configured runtime limits for worker memory and background work SHALL affect actual scheduling or eviction behavior.

#### Scenario: Memory limit influences eviction
- **WHEN** worker memory pressure exceeds the configured limit
- **THEN** `clice` uses the configured limit to drive eviction or rejection behavior

### Requirement: Background work does not outrank active editing
`clice` SHALL schedule compile and indexing work so that opened-document responsiveness takes priority over background throughput.

#### Scenario: Open document work outranks background indexing
- **WHEN** a user edits an opened document while background indexing is pending
- **THEN** `clice` prioritizes the opened-document work ahead of non-urgent background indexing

### Requirement: Production debugging information is observable
`clice` SHALL emit enough logging and trace information to diagnose compile-context selection, worker failures, and request/response mismatches in production-style usage.

#### Scenario: Compile failure includes useful context
- **WHEN** a compile, header-context selection, or worker request fails
- **THEN** logs include enough context to identify the affected file, command/context, and failure site
