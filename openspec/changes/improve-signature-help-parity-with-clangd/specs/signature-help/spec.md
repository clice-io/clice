## ADDED Requirements

### Requirement: Server advertises signature help trigger coverage
The server SHALL advertise a signature help provider that includes the trigger and retrigger characters needed to keep signature help updated while the user edits callable and initializer argument lists.

#### Scenario: Initialize returns signature help triggers
- **WHEN** a client initializes against the server
- **THEN** the advertised `signatureHelpProvider` includes trigger support for `(`, `)`, `{`, `}`, `<`, `>`, and `,`

### Requirement: Signature help returns overload sets and the active parameter
The server SHALL return all applicable signature candidates for the callable at the cursor and SHALL report the active signature and active parameter for the call being edited.

#### Scenario: Overloaded function call
- **WHEN** the cursor is inside a call to an overloaded function
- **THEN** the result contains each viable overload signature
- **THEN** the result identifies the active parameter for the preferred signature

### Requirement: Signature help maps arguments to rendered parameters across C++ call forms
The server SHALL remap the current argument index to the correct rendered parameter for variadic, defaulted, templated, constructor, aggregate, and function-pointer signatures instead of exposing raw argument indices when they exceed or differ from the displayed parameter list.

#### Scenario: Variadic call stays on the variadic parameter
- **WHEN** the user types beyond the fixed parameters of a variadic callable
- **THEN** the reported active parameter remains the final variadic parameter instead of advancing past the signature

#### Scenario: Constructor or aggregate initializer
- **WHEN** the cursor is inside a constructor call or aggregate braced initializer
- **THEN** the result describes the constructor or aggregate fields being initialized
- **THEN** the reported active parameter matches the current initializer slot

### Requirement: Signature help returns stable parameter metadata and documentation
The server SHALL return parameter label metadata aligned with the rendered signature label, and SHALL include signature documentation in the format negotiated with the client when documentation is available from AST comments or indexed symbols.

#### Scenario: Client supports label offsets and markdown documentation
- **WHEN** the client declares parameter label offset support and markdown signature documentation
- **THEN** returned signatures include offset-based parameter labels
- **THEN** returned signatures include markdown-formatted documentation when documentation is available

#### Scenario: Callable without documentation
- **WHEN** a callable has no available documentation
- **THEN** the server still returns the signature label and parameter metadata without failing the request

### Requirement: Signature help works for imported declarations and nested expressions
The server SHALL provide signature help for call sites that resolve through imported or prerequisite-module declarations and for cursors nested inside expressions within argument lists.

#### Scenario: Imported module declaration
- **WHEN** the user requests signature help for a function imported from a prerequisite C++20 module
- **THEN** the result includes the imported declaration's signature

#### Scenario: Cursor inside nested argument expression
- **WHEN** the cursor is inside an expression that itself appears in an argument position
- **THEN** the server reports signature help for the innermost active call
- **THEN** the reported active parameter matches that nested call site
