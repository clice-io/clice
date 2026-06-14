# Contribution

We welcome any contributions!

Please refer to [build](./build.md) to build clice, and [test and debug](./test-and-debug.md) to run tests.

## Before Submitting

1. **Format**: Run `pixi run format` to format all source files.
2. **Tests**: All three test suites must pass (`pixi run test`).
3. **Commit messages**: Use [conventional commits](https://www.conventionalcommits.org/):

    ```
    <type>(<scope>): <short description>
    ```

    Types: `feat`, `fix`, `refactor`, `chore`, `docs`, `ci`, `test`, `perf`

    Scopes: match `src/` subdirectories or feature names (e.g., `completion`, `server`, `index`).

    Keep the subject line under 70 characters.

## Code Style

### Naming

| Entity | Convention | Example |
|--------|-----------|---------|
| Variables, fields, functions | `snake_case` | `file_path`, `apply_defaults` |
| Classes, enums, concepts | `PascalCase` | `CompileGraph`, `SymbolKind` |
| Enum values | `PascalCase` | `GoToDefinition`, `IncludeAngled` |
| Template parameters | `PascalCase` | `typename Result` |

Class member fields use no prefix or suffix (no `m_`, no trailing `_`).

### String Parameters

Prefer `llvm::StringRef` > `std::string_view` > `const std::string&`.

### Modern C++

- Target C++23. Use `std::ranges`, `std::expected`, `std::format`.
- Prefer LLVM data structures (`SmallVector`, `DenseMap`, `StringRef`) where appropriate.
- Do not use `<iostream>` or C-style I/O.
- Prefer raw string literals `R"(...)"` over escaped strings.

### Error Handling

- Use `if` with init-statements to scope error variables.
- Keep early-return / flat control flow — avoid deep nesting.

## Extension Development

See [extension](./extension.md) for editor plugin development.
