## ADDED Requirements

### Requirement: Rich semantic token classification
The server SHALL classify semantic tokens using AST-derived symbol kinds and modifiers when that information is available, instead of limiting output to lexical token classes and basic declaration markers.

#### Scenario: Declaration kinds use semantic classification
- **WHEN** a document contains declarations and references for namespaces, records, enums, functions, methods, fields, variables, parameters, labels, concepts, or macros
- **THEN** the semantic token response MUST encode those occurrences with the corresponding `SymbolKind` rather than a generic lexical fallback

#### Scenario: Semantic modifiers are preserved
- **WHEN** a token represents a declaration, definition, template-related symbol, const-qualified symbol, overloaded symbol, or typed punctuation that `clice` can determine from the AST
- **THEN** the semantic token response MUST include the corresponding `SymbolModifiers` bits for that token

#### Scenario: Semantic meaning wins over lexical overlap
- **WHEN** lexical and semantic passes produce overlapping classifications for the same source token
- **THEN** the server MUST prefer the most specific semantic classification and MUST NOT degrade the final response to `Conflict` for normal supported cases

### Requirement: C++20 module-aware semantic highlighting
The server SHALL emit semantic tokens for C++20 module declarations and imports so module names and partitions are distinguishable from ordinary identifiers.

#### Scenario: Named module declaration is highlighted
- **WHEN** a module interface or implementation unit contains a named module declaration such as `export module Foo;` or `module Foo;`
- **THEN** the `module` keyword MUST be highlighted as a keyword and the module-name tokens MUST be highlighted with `SymbolKind::Module`

#### Scenario: Module import is highlighted
- **WHEN** a translation unit imports a named module or partition such as `import Foo;` or `import Foo:Bar;`
- **THEN** the `import` keyword MUST be highlighted as a keyword and the imported module-name tokens MUST be highlighted with `SymbolKind::Module`

#### Scenario: Non-module fragments do not invent module-name tokens
- **WHEN** a file contains module-related fragment syntax such as `module;` or `module :private;`
- **THEN** the server MUST highlight the language keywords normally and MUST only emit `SymbolKind::Module` tokens for the actual named module identifiers that exist in source

### Requirement: Semantic token incremental delivery
The server SHALL support stable semantic token result IDs and delta responses for documents whose previous semantic token result is known.

#### Scenario: Full request returns a result identifier
- **WHEN** a client sends `textDocument/semanticTokens/full`
- **THEN** the response MUST include the full semantic token data for the current document snapshot and a `resultId` that can be used for a later delta request

#### Scenario: Delta request returns edits when previous result matches
- **WHEN** a client sends `textDocument/semanticTokens/full/delta` with the latest known `resultId` for a document
- **THEN** the server MUST return semantic token edits relative to that previous result instead of forcing a full token payload

#### Scenario: Delta request falls back to full tokens when history is unavailable
- **WHEN** a client sends `textDocument/semanticTokens/full/delta` with an unknown or stale `resultId`
- **THEN** the server MUST return a full semantic token result for the current document snapshot

#### Scenario: Rebuild-driven semantic changes trigger refresh
- **WHEN** recompilation, dependency invalidation, or module rebuild changes the semantic token output for an open document and the client supports semantic token refresh
- **THEN** the server MUST request a semantic token refresh so the client can fetch updated tokens
