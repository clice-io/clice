# Folding Ranges

## Fold Kinds

- [x] Block folding — functions, classes, structs, unions, enums, namespaces, compound statements, lambdas
- [x] Multi-line list folding — function parameters, call arguments, initializer lists, lambda captures

  ```cpp
  void configure(
      int width,           // ┐
      int height,          // │ foldable parameter list
      bool fullscreen      // ┘
  );

  auto result = compute(
      getWidth(),          // ┐
      getHeight(),         // │ foldable argument list
      true                 // ┘
  );
  ```

- [x] Access-specifier section folding — `public:` / `protected:` / `private:` regions within a class ([clangd#1455](https://github.com/clangd/clangd/issues/1455))

  ```cpp
  class Widget {
  public:            // ┐
      void draw();   // │ foldable
      void resize(); // ┘
  private:           // ┐
      int width;     // │ foldable
      int height;    // ┘
  };
  ```

- [x] Preprocessor conditional folding (`#if` / `#ifdef` / `#ifndef` ... `#endif`) ([clangd#1661](https://github.com/clangd/clangd/issues/1661); [clangd#2059](https://github.com/clangd/clangd/issues/2059) is a duplicate of #1661)
- [x] Custom region folding (`#pragma region` / `#pragma endregion`) ([clangd#1623](https://github.com/clangd/clangd/issues/1623))
- [ ] Comment folding — multi-line `/* */` and consecutive `//` line comments

  ```cpp
  // This is a long
  // multi-line comment
  // that should fold as one region

  /*
   * Block comment
   * should also fold
   */
  ```

- [ ] Include region folding — consecutive `#include` directives

  ```cpp
  #include <vector>       // ┐
  #include <string>       // │ foldable region
  #include <algorithm>    // ┘

  #include "app.h"        // ┐ separate region
  #include "config.h"     // ┘ (blank line separates)
  ```

- [ ] Raw string literal folding

  ```cpp
  auto sql = R"(
      SELECT *
      FROM users
      WHERE active = true
  )";  // foldable multi-line raw string
  ```

- [ ] `using` declaration blocks — consecutive using declarations/directives

  ```cpp
  using std::vector;  // ┐
  using std::string;  // │ foldable
  using std::map;     // ┘
  ```

- [ ] Template parameter list folding

  ```cpp
  template<
      typename Key,            // ┐
      typename Value,          // │ foldable
      typename Compare = less  // ┘
  >
  class SortedMap { };
  ```

## Refinements

- [x] `collapsedText` placeholder (LSP 3.17) — show a summary when folded ([clangd#2667](https://github.com/clangd/clangd/issues/2667))

  ```
  void processData(const Config& cfg) {...}   // shows signature + {...}
  #include <vector>  ... (5 more)             // shows include count
  /* License header... */                      // shows first line
  ```

  > **Client support**: VS Code does **not** support `collapsedText` yet ([vscode#70794](https://github.com/microsoft/vscode/issues/70794) — still open); Neovim with nvim-lsp supports it natively. Clients that do not implement this field will silently ignore it — the folding still works, only the placeholder text is missing.

- [ ] Fold from the declaration line for function/class bodies — keep the signature visible when folded ([clangd#2666](https://github.com/clangd/clangd/issues/2666))

  ```cpp
  // folded: void processData(const Config& cfg) {...}
  // not:    {... (signature hidden above fold)}
  ```

  > **Client support**: this depends on the client interpreting `FoldingRange.startLine` correctly. VS Code uses the line _after_ `startLine` as the first hidden line, so setting `startLine` to the declaration line achieves the desired effect. However, VS Code still leaves the closing `}` on a separate line rather than collapsing it onto the signature line ([vscode#3352](https://github.com/microsoft/vscode/issues/3352) — still open). Other clients may differ.

- [ ] Inactive preprocessor branch indication — visually distinguish or auto-fold inactive `#if`/`#else` branches

  ```cpp
  #ifdef _WIN32
      // ... Windows code (active) ...
  #else
      // ... POSIX code (inactive, could auto-fold) ...
  #endif
  ```

  > **Note**: this overlaps with semantic tokens (inactive code dimming) and is partly a client UX concern. The server can mark these ranges with `FoldingRangeKind.Region` and clients can choose to auto-fold them.

## Changelog

| Date | Change                                                               | PR  |
| ---- | -------------------------------------------------------------------- | --- |
| —    | Block folding, list folding, access specifiers, preprocessor regions | —   |
