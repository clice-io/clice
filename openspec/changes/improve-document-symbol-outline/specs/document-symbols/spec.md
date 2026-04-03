## ADDED Requirements

### Requirement: Document outline excludes non-structural local declarations
The document symbol response SHALL include user-written document-structure declarations from the main file, including namespaces, records, enums, fields, top-level variables, functions, methods, constructors, destructors, conversion functions, deduction guides, concepts, and structured bindings.

The document symbol response SHALL NOT include declarations that are local to a function or method body, and SHALL NOT include implicit declarations or implicit template instantiations.

#### Scenario: Local variable does not appear in outline
- **WHEN** a function body contains a local variable declaration and a local class declaration
- **THEN** the function itself appears in document symbols
- **THEN** the local variable and local class do not appear as document symbol children

#### Scenario: Member hierarchy is preserved
- **WHEN** a record contains nested records, fields, methods, and enum members
- **THEN** the outer record appears in document symbols
- **THEN** the nested declarations appear as children of that record in source order

### Requirement: Template specializations follow written-source structure
The document symbol response SHALL ignore implicit template instantiations.

The document symbol response SHALL include user-written explicit specializations and explicit instantiations exactly once. Explicit specializations that define nested members SHALL expose those nested members as children. Explicit instantiations SHALL appear as leaf symbols unless the source itself contains nested declarations at that location.

#### Scenario: Explicit specialization exposes children
- **WHEN** a class template has an explicit specialization with user-written members in the main file
- **THEN** the explicit specialization appears in document symbols
- **THEN** its user-written members appear as children of that specialization

#### Scenario: Explicit instantiation is a leaf
- **WHEN** the main file contains an explicit template instantiation declaration or definition
- **THEN** the instantiated symbol appears in document symbols exactly once
- **THEN** it does not synthesize member children that are not written at the instantiation site

### Requirement: Macro-expanded declarations are grouped by macro invocation
When a declaration included in the outline originates from a macro expansion whose invocation is written directly in the main file, the document symbol response SHALL group the expanded declarations under a macro container symbol associated with that invocation.

The macro container SHALL appear at the invocation site and SHALL contain all outline declarations produced by that invocation within the current scope.

#### Scenario: Function-like macro groups expanded declarations
- **WHEN** a function-like macro invocation in the main file expands to a class declaration with members
- **THEN** document symbols include a container node for that macro invocation
- **THEN** the expanded class appears as a child of that container node

#### Scenario: One macro invocation creates one container
- **WHEN** a single macro invocation expands to multiple top-level declarations
- **THEN** document symbols include one container node for that invocation
- **THEN** all declarations produced by that invocation appear beneath the same container node

### Requirement: Document symbol ranges are editor-safe
Document symbols SHALL be returned in source order within each sibling list.

For every returned symbol, `selectionRange` SHALL be contained within `range`. For every parent symbol with children, the parent `range` SHALL contain each child `range`, including declarations originating from macro expansions.

#### Scenario: Selection range is repaired when written name range is inconsistent
- **WHEN** a declaration's preferred name location would produce a `selectionRange` outside its declaration `range`
- **THEN** the response repairs the symbol ranges so that `selectionRange` is contained within `range`

#### Scenario: Parent range contains nested child symbols
- **WHEN** a symbol has nested child declarations whose ranges extend beyond the initially collected parent range
- **THEN** the parent symbol range is expanded so that all child ranges are contained within it
