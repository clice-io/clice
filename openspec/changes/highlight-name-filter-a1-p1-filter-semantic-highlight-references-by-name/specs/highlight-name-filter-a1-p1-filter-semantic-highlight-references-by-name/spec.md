## Request Metadata

- Proposal Title: Filter semantic highlight references by name eligibility
- Canonical Request Title: `fix(semantic-tokens): filter ineligible highlight references`
- Conventional Title: `fix(semantic-tokens)`

## ADDED Requirements

### Requirement: Semantic token references require highlightable declaration names
The semantic-token collector SHALL emit reference tokens only when the referenced `NamedDecl` has a declaration name that is eligible for source highlighting. The eligibility rule MUST mirror clangd's `canHighlightName` behavior by allowing non-empty identifier names plus constructor and destructor names, and by rejecting Objective-C selectors, conversion functions, overloaded operators, deduction guides, literal operators, and using directives.

#### Scenario: Unsupported declaration-name references are suppressed
- **WHEN** semantic-token collection visits a reference occurrence whose target declaration has an ineligible declaration-name kind such as an overloaded operator
- **THEN** the collector emits no semantic token for that reference occurrence

#### Scenario: Constructor references remain highlighted
- **WHEN** semantic-token collection visits a reference occurrence whose target declaration is a constructor
- **THEN** the collector emits a semantic token for the constructor reference using the existing constructor/destructor token classification

#### Scenario: Destructor references remain highlighted
- **WHEN** semantic-token collection visits a reference occurrence whose target declaration is a destructor
- **THEN** the collector emits a semantic token for the destructor reference using the existing constructor/destructor token classification
