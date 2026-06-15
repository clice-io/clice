# Signature Help

Implementation: `src/feature/signature_help.cpp`

## Core Functionality

Forwarded to Clang `CodeCompleteConsumer::ProcessOverloadCandidates` via stateless worker.

- [x] Function overload signatures
- [x] Active parameter tracking
- [x] Template instantiation pattern resolution (shows template, not instantiation)
- [x] Clean printing policy (no anonymous tag locations, suppress scopes)

## Trigger Characters

Registered: `(`, `)`, `{`, `}`, `<`, `>`, `,`

| Character | Context            | Behavior                 |
| --------- | ------------------ | ------------------------ |
| `(`       | Function call      | Show overload signatures |
| `)`       | Close paren        | Update context           |
| `{`       | Brace init         | Show overload signatures |
| `}`       | Close brace        | Update context           |
| `<`       | Template args      | Show overload signatures |
| `>`       | Template close     | Update context           |
| `,`       | Argument separator | Update active parameter  |

## Display

- [x] Parameter labels with types
- [x] Return type in signature label
- [ ] Documentation for active parameter
- [ ] Overload set count indicator
