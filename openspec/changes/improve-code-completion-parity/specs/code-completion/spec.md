## ADDED Requirements

### Requirement: Capability-aware completion responses
The server SHALL shape code-completion responses according to client capabilities, trigger context, and requested limits instead of returning one fixed completion-item format to every client.

#### Scenario: Snippet-capable client receives snippet completions
- **WHEN** the client advertises snippet support and a completion candidate has callable arguments, template arguments, or other structured suffix text
- **THEN** the completion response MUST encode that candidate using snippet-formatted insertion text rather than degrading it to plain text

#### Scenario: Plain-text client receives compatible completion items
- **WHEN** the client does not advertise snippet support or label-details support
- **THEN** the server MUST return completion items that remain correct for that client and MUST NOT require unsupported snippet or label-details fields to preserve meaning

#### Scenario: Completion result limit is enforced
- **WHEN** the client or server applies a completion result limit and more matching candidates exist than can be returned
- **THEN** the server MUST truncate the response to the chosen limit and MUST mark the completion list as incomplete

#### Scenario: Spurious auto-triggered completion is suppressed
- **WHEN** completion is auto-triggered by a trigger character in a syntactic position that cannot produce meaningful completion candidates
- **THEN** the server MUST return an empty completion list instead of forcing a normal completion run

### Requirement: Multi-source candidate collection and semantic ranking
The server SHALL build completion results from multiple candidate sources and rank them using semantic/contextual signals rather than using only raw label matching.

#### Scenario: Sema and index candidates are merged by insertion semantics
- **WHEN** Sema and project-index collection both produce candidates that insert the same symbol in the same way
- **THEN** the server MUST return a single merged completion item rather than duplicate visible entries

#### Scenario: Local identifiers supplement Sema completion
- **WHEN** the open document contains matching identifiers that are not surfaced by Sema at the completion point
- **THEN** the server MUST be able to return those identifiers as completion candidates when they are relevant to the typed prefix

#### Scenario: Required qualifiers are preserved for out-of-scope symbols
- **WHEN** a project-index completion candidate is not directly visible at the completion point but remains a valid suggestion through qualification
- **THEN** the completion item MUST preserve the required qualifier in its rendered insertion text instead of pretending the symbol is already in scope

#### Scenario: Semantic relevance outranks weaker textual matches
- **WHEN** two otherwise similar candidates match the typed prefix but only one is strongly favored by semantic/contextual signals such as active scope, expected type, or availability in the current compilation context
- **THEN** the semantically favored candidate MUST rank above the weaker match

### Requirement: Rich completion item rendering and completion-related edits
The server SHALL render completion items with enough metadata and edits to make them useful in modern editors, including signatures, detail fields, filtering/sorting metadata, documentation, deprecation, and completion-associated edits.

#### Scenario: Function completion includes signature and return-type detail
- **WHEN** a callable declaration is returned as a completion candidate
- **THEN** the completion item MUST expose its callable signature and MUST expose return-type or overload-summary detail in the rendered result

#### Scenario: Documentation and deprecation are preserved
- **WHEN** a completion candidate has documentation text or is marked deprecated by Sema or index metadata
- **THEN** the completion item MUST surface the documentation and deprecation state to the client

#### Scenario: Sema fix-its are attached to the completion item
- **WHEN** Sema provides fix-it edits that are required or strongly suggested for a completion candidate
- **THEN** the server MUST attach those edits to the completion item so the accepted completion can apply them

#### Scenario: Dependency insertion edits are attached when justified
- **WHEN** a completion candidate refers to a symbol that requires adding a missing dependency and the completion source can identify the correct insertion form
- **THEN** the completion item MUST attach the corresponding dependency insertion edits instead of returning only the symbol name

### Requirement: Context-sensitive completion for headers, modules, and templates
The server SHALL use `clice`'s compilation-context machinery so completion reflects the active header context, module dependency state, and template-instantiation information instead of treating all files as context-free.

#### Scenario: Shared header completion follows the active header context
- **WHEN** the same header is completed under different active includer/source contexts
- **THEN** the completion results MUST reflect the active header context rather than a context-free union of all possible includers

#### Scenario: Module-aware completion uses available PCM dependencies
- **WHEN** the active file depends on C++20 modules whose PCMs are available to the completion worker
- **THEN** the completion pipeline MUST consider symbols made visible by those module dependencies when producing results

#### Scenario: Module dependency insertion prefers import-aware edits
- **WHEN** the selected completion candidate is best satisfied by adding a module dependency rather than a textual include and the active translation unit supports that form
- **THEN** the completion item MUST use a module-aware dependency insertion edit instead of forcing a header-style include edit

#### Scenario: Template-instantiation-aware ranking prefers concretely compatible candidates
- **WHEN** the active completion site provides concrete template-instantiation or expected-type information that distinguishes candidate relevance
- **THEN** the ranking logic MUST prefer candidates compatible with that instantiated context over otherwise similar generic alternatives
