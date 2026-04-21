## ADDED Requirements

### Requirement: Highlightable declaration occurrences are filtered by declaration name kind
The system SHALL emit highlightable definition and reference occurrences for a `NamedDecl` only when its `DeclarationName` has a concrete highlightable spelling. Non-empty identifiers, constructors, and destructors MUST remain eligible. Anonymous or empty identifiers, ObjC selectors, conversion functions, overloaded operators, deduction guides, literal operators, and using-directive names MUST NOT produce highlightable declaration occurrences.

#### Scenario: Ordinary identifiers and constructor-style names remain highlightable
- **WHEN** clice builds semantic-token or index-backed highlight occurrences for a declaration with a non-empty identifier, constructor name, or destructor name
- **THEN** the corresponding declaration or reference range is emitted as a highlightable occurrence

#### Scenario: Unhighlightable declaration names are skipped
- **WHEN** clice encounters an anonymous or empty identifier, ObjC selector, conversion function, overloaded operator, deduction guide, literal operator, or using-directive name
- **THEN** it does not emit a highlightable declaration occurrence for that source location

### Requirement: Skipping highlightable occurrences does not remove unrelated relation data
The system SHALL preserve non-highlight semantic and index relations when a declaration name fails the highlightability check. Filtering an unhighlightable declaration occurrence MUST NOT remove otherwise valid declaration, definition, reference, or call relations for other symbols in the same translation unit.

#### Scenario: Index relations remain available around skipped names
- **WHEN** a translation unit contains an unhighlightable declaration name and other symbols that still participate in declaration, definition, reference, or call relations
- **THEN** the unhighlightable name is absent from highlightable occurrence ranges
- **AND** the unrelated relation data for the other symbols remains queryable
