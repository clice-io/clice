# Folding Ranges

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/data/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `python tests/tools/feature_docs.py update`. -->

## Fold Kinds

<!-- BEGIN GENERATED ITEMS: fold_kinds -->

- [x] Block folding — functions, classes, structs, unions, enums, namespaces, lambdas

  ```cpp
  namespace geometry {

  enum class Shape {
      Circle,
      Square,
      Triangle
  };

  struct Point {
      int x;
      int y;
  };

  union Value {
      int as_int;
      float as_float;
  };

  class Canvas {
      Point origin;

      int area() {
          auto scale = [](int factor) {
              return factor * 2;
          };
          return scale(4);
      }
  };

  }  // namespace geometry
  ```

- [ ] Nested compound-statement folding — `if`/`for`/`while` bodies inside functions

  ```cpp
  void process(int count) {
      if (count > 0) {                       // ┐
          for (int i = 0; i < count; ++i) {  // │ nested blocks that could
              // ... work ...                // │ fold independently of
          }                                  // │ the enclosing function
      }                                      // ┘
  }
  ```

- [x] Multi-line list folding — function parameters, call arguments, initializer lists, lambda captures

  ```cpp
  void configure(
      int width,       // ┐
      int height,      // │ foldable parameter list
      bool fullscreen  // ┘
  );

  int compute(int a, int b, int c);

  void demo() {
      int values[] = {
          1,  // ┐
          2,  // │ foldable initializer list
          3   // ┘
      };

      int result = compute(
          values[0],  // ┐
          values[1],  // │ foldable argument list
          values[2]   // ┘
      );

      auto sum = [
          first = values[0],   // ┐
          second = values[1]   // ┘ foldable lambda capture
      ] {
          return first + second;
      };
  }
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

- [ ] Preprocessor conditional folding (`#if` / `#ifdef` / `#ifndef` ... `#endif`) _(partial)_ ([clangd#1661](https://github.com/clangd/clangd/issues/1661), [clangd#2059](https://github.com/clangd/clangd/issues/2059))

  Branch regions delimited by `#else` fold today; a bare `#if ... #endif`
  block without an `#else` does not fold yet. clangd#2059 is a duplicate
  of clangd#1661.

  ```cpp
  #ifdef ENABLE_LOGGING    // ┐
  void log_message();      // │ no fold yet: bare conditional without #else
  #endif                   // ┘

  #ifdef USE_THREADS       // ┐
  void spawn_workers();    // │ folds: branches delimited by #else
  #else                    // │
  void run_inline();       // │
  #endif                   // ┘
  ```

- [x] Custom region folding (`#pragma region` / `#pragma endregion`) ([clangd#1623](https://github.com/clangd/clangd/issues/1623))

  ```cpp
  #pragma region Configuration

  int retry_count = 3;
  int timeout_ms = 5000;

  #pragma endregion
  ```

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
  template<typename T>
  struct Less;

  template<
      typename Key,                 // ┐
      typename Value,               // │ foldable
      typename Compare = Less<Key>  // ┘
  >
  class SortedMap { };
  ```

<!-- END GENERATED ITEMS -->

## Refinements

<!-- BEGIN GENERATED ITEMS: refinements -->

- [x] `collapsedText` placeholder (LSP 3.17) — show a summary when folded ([clangd#2667](https://github.com/clangd/clangd/issues/2667))

  > **Client support**: VS Code does **not** support `collapsedText` yet
  > ([vscode#70794](https://github.com/microsoft/vscode/issues/70794) — still
  > open); Neovim with nvim-lsp supports it natively. Clients that do not
  > implement this field will silently ignore it — the folding still works,
  > only the placeholder text is missing.

  ```cpp
  struct Config {
      int width;
      int height;
  };

  // When folded, the body collapses to a `{...}` placeholder while the
  // signature stays visible: int process_data(const Config& cfg) {...}
  int process_data(const Config& cfg) {
      return cfg.width * cfg.height;
  }
  ```

- [ ] Fold from the declaration line for function/class bodies — keep the signature visible when folded ([clangd#2666](https://github.com/clangd/clangd/issues/2666))

  > **Client support**: this depends on the client interpreting
  > `FoldingRange.startLine` correctly. VS Code uses the line _after_
  > `startLine` as the first hidden line, so setting `startLine` to the
  > declaration line achieves the desired effect. However, VS Code still
  > leaves the closing `}` on a separate line rather than collapsing it onto
  > the signature line ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)
  > — still open). Other clients may differ.

  ```cpp
  struct Config {
      int width;
      int height;
  };

  // desired when folded: int process_data(const Config& cfg) {...}
  // not:                 {... (signature hidden above fold)}
  int process_data(const Config& cfg) {
      int area = cfg.width * cfg.height;
      return area;
  }
  ```

- [ ] Inactive preprocessor branch indication — visually distinguish or auto-fold inactive `#if`/`#else` branches _(partial)_

  The server emits a fold range for the region between the condition and
  `#else`, so the first branch can be folded manually; the post-`#else`
  branch gets no range yet. Knowing which branch is _inactive_ — to dim or
  auto-fold it — is not implemented here; that information belongs to the
  inactive-regions feature.

  > **Note**: this overlaps with semantic tokens (inactive code dimming) and
  > is partly a client UX concern. The server can mark these ranges with
  > `FoldingRangeKind.Region` and clients can choose to auto-fold them.

  ```cpp
  #ifdef _WIN32
      // ... Windows code (active) ...
  #else
      // ... POSIX code (inactive, could auto-fold) ...
  #endif
  ```

<!-- END GENERATED ITEMS -->

## Changelog

| Date | Change                                                               | PR  |
| ---- | -------------------------------------------------------------------- | --- |
| —    | Block folding, list folding, access specifiers, preprocessor regions | —   |
