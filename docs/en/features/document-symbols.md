# Document Symbols

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`.

## Symbol Hierarchy

- [x] Nested document symbol tree (parent-child relationships)
- [x] Symbol ranges and selection ranges
- [x] UTF-16 position encoding
- [ ] Access specifier nodes in the symbol tree — show `public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation ([clangd#499](https://github.com/clangd/clangd/issues/499))

  ```
  Widget (class)
  ├─ public
  │  ├─ draw() (method)
  │  └─ resize() (method)
  └─ private
     ├─ width (field)
     └─ height (field)
  ```

- [ ] Anonymous namespace / unnamed struct grouping

  ```
  (anonymous namespace) (namespace)
  └─ helper() (function)
  ```

## Symbol Kinds

- [x] Namespace
- [x] Class / Struct / Union
- [x] Enum / Enum member
- [x] Function / Method / Constructor
- [x] Variable / Field / Binding
- [x] Template declarations (via inner templated entity)
- [ ] Typedef / Type alias
- [ ] Concept

## Symbol Detail

- [ ] Function signatures in the `detail` field — parameter types and names for overload disambiguation ([clangd#520](https://github.com/clangd/clangd/issues/520), [clangd#601](https://github.com/clangd/clangd/issues/601), [clangd#1232](https://github.com/clangd/clangd/issues/1232))

  ```
  // outline without detail:
  process (function)        ← which overload?
  process (function)

  // outline with detail:
  process(int x) (function)
  process(std::string s) (function)
  ```

- [ ] Variable type in the `detail` field

  ```
  // outline: "timeout" detail: "int"
  // outline: "logger" detail: "std::shared_ptr<Logger>"
  ```

- [ ] Base class in the `detail` field for class declarations

  ```
  // outline: "Circle" detail: ": Shape"
  ```

- [ ] Strip default parameter values from signatures in outline ([clangd#221](https://github.com/clangd/clangd/issues/221))

  ```
  // source:  void open(std::string path, int mode = 0644);
  // outline: open(string path, int mode) — no "= 0644"
  ```

- [ ] Correct symbol range for multiline function signatures — the range should include the full signature so VS Code sticky scroll works ([clangd#2221](https://github.com/clangd/clangd/issues/2221))

  ```cpp
  void Widget::processData(       // ← symbol range starts here
      const Config& cfg,
      int flags
  ) {                              // ← not here
  ```

## Missing Symbols

- [ ] Macro definitions in the outline ([clangd#1744](https://github.com/clangd/clangd/issues/1744))

  ```
  MAX_BUFFER_SIZE (macro)
  CHECK(cond, msg) (macro)
  ```

- [ ] Include directives in the outline ([clangd#2226](https://github.com/clangd/clangd/issues/2226))

  ```
  #include <vector> (include)
  #include "config.h" (include)
  ```

- [ ] Local variables inside function bodies ([clangd#616](https://github.com/clangd/clangd/issues/616))
- [ ] Module declarations (`module`, `import`, `export module`)

## Symbol Tags

- [ ] Deprecated tag for `[[deprecated]]` symbols
- [ ] Access modifier indication (public / private / protected) ([clangd#2123](https://github.com/clangd/clangd/issues/2123))
- [ ] Static / virtual / abstract indicators

## Location Correctness

- [ ] Correct source location for symbols defined inside macros ([clangd#475](https://github.com/clangd/clangd/issues/475))

  ```cpp
  #define DEFINE_HANDLER(name) void name()
  DEFINE_HANDLER(onReady);  // outline should navigate to this line, not the macro definition
  ```

- [ ] Selection range for macro-defined variables should point to the variable name, not the macro token ([clangd#1941](https://github.com/clangd/clangd/issues/1941))

## Changelog

| Date | Change                                      | PR  |
| ---- | ------------------------------------------- | --- |
| —    | Nested symbol hierarchy, basic symbol kinds | —   |
