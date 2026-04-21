# Filter non-highlightable definition references

- Canonical Request Title: `fix(highlight/references): filter non-highlightable definition references`
- Conventional Title: `fix(highlight/references)`

## ADDED Requirements

### Requirement: Non-highlightable definitions are excluded from definition-origin reference highlighting

The system SHALL suppress indexed reference-highlighting results when the origin symbol resolves to a definition whose declaration name is not highlightable. Non-highlightable names include declarations without a written identifier token and clang `DeclarationName` kinds that follow the requested `canHighlightName` rejection policy, such as overloaded operators, conversion functions, deduction guides, literal operators, Objective-C selectors, and using directives.

#### Scenario: Anonymous or operator-like definition

- **WHEN** a definition-origin reference-highlighting query resolves to a symbol whose indexed metadata marks its declaration name as non-highlightable
- **THEN** the query returns no indexed reference-highlight locations for that symbol

### Requirement: Highlightable definitions remain eligible for reference highlighting

The system SHALL continue returning indexed reference-highlighting results for definitions whose declaration names are highlightable, including non-empty identifiers, constructors, and destructors.

#### Scenario: Identifier definition

- **WHEN** a definition-origin reference-highlighting query resolves to a definition with a non-empty identifier name and indexed references
- **THEN** the query returns the indexed reference-highlight locations for that symbol

#### Scenario: Constructor or destructor definition

- **WHEN** a definition-origin reference-highlighting query resolves to a constructor or destructor definition with indexed references
- **THEN** the query returns the indexed reference-highlight locations for that symbol

### Requirement: Highlightability is preserved across index persistence

The system SHALL persist whether a symbol's declaration name is highlightable through translation-unit indexing, project-index merge, serialized index storage, and reload so query-time filtering does not depend on live AST access.

#### Scenario: Reloaded project index

- **WHEN** a symbol is indexed, persisted, and later queried through the reloaded project index without re-entering the AST
- **THEN** the query uses the stored highlightable-name metadata to decide whether indexed reference-highlight locations are returned

### Requirement: Non-highlight consumers keep the underlying reference relations

The system SHALL apply the highlightability check in the highlight-oriented query path without removing the underlying indexed reference relations needed by other consumers.

#### Scenario: Index build stores relations for a non-highlightable definition

- **WHEN** a symbol with a non-highlightable definition name is indexed
- **THEN** the index retains its stored reference relations so non-highlight-specific consumers can continue using them
