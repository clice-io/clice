# Folding Ranges

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/data/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/feature_docs.ts update`. -->

## Fold Kinds

<!-- BEGIN GENERATED ITEMS: Fold Kinds -->

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

  namespace spaced
  {

  struct Placeholder {
      int filler;
  };

  }  // namespace spaced
  ```

- [x] Nested compound-statement folding — `if`/`for`/`while` bodies inside functions

  ```cpp
  void process(int count) {
      if (count > 0) {
          for (int i = 0; i < count; i += 1) {
              count -= 1;
          }
      }

      while (count > 0) {
          count -= 1;
      }

      // A bare scope block folds too.
      {
          int scratch = count;
          count = scratch + 1;
      }
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

      auto scale = [](
          int base,    // ┐ foldable lambda
          int factor   // ┘ parameter list
      ) {
          return base * factor;
      };

      result += sum() + scale(result, 2);
  }

  int accumulate(
      int start,  // ┐
      int step,   // │ foldable parameter list
      int count   // ┘ on a definition
  ) {
      return start + step * count;
  }

  void log_all(
      const char* format,  // ┐ variadic parameter
      ...                  // ┘ list still folds
  );

  struct Rect {
      Rect(int w, int h);
  };

  Rect area(
      10,  // ┐ foldable constructor
      20   // ┘ arguments
  );

  Rect brace_area{
      30,
      40
  };
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

- [x] Pragma classification — only the first argument token decides region/endregion

  ```cpp
  // The leading declaration ends the preamble so the pragmas below reach the
  // main-file parse on both the inspect and the server path.
  int before = 0;

  // Neither a region name nor another pragma's argument mentioning
  // "endregion" may close the fold early.
  #pragma region endregion_pair
  int retries = 3;
  #pragma mark see endregion notes
  int limit = 10;
  #pragma endregion

  // The tail of a multiline comment before the introducer must not hide
  // the region either.
  /* spans
  a line */ #pragma region after_comment
  int after = 1;
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

- [x] Template specializations and instantiations — written specializations and their members fold; instantiated declarations reuse the pattern's source locations and must not fold it again

  ```cpp
  template <typename T>
  struct Box {
      T value;

      void reset() {
          value = T();
      }
  };

  template <>
  struct Box<void> {
      void reset() {
          // nothing stored
      }
  };

  template <typename T>
  struct Box<T*> {
      T* pointee;
  };

  // Neither the implicit instantiation Box<int> nor the explicit instantiation
  // Box<char> re-folds the primary's braces or the reset() body.
  Box<int> implicit_use;
  template struct Box<char>;
  ```

- [x] Abbreviated function templates — bodies of functions with `auto` or constrained `auto` parameters fold like any other function

  ```cpp
  template <typename T>
  concept Small = sizeof(T) <= 8;

  void consume(Small auto x) {
      auto copy = x;
      copy += 1;
  }

  void forward(auto value) {
      consume(value);
  }
  ```

- [x] Macro-generated folding — braces and access specifiers spelled through macros fold at the invocation site

  ```cpp
  #define NS_BEGIN namespace ns {
  #define NS_END }
  #define PUBLIC public:
  #define PRIVATE private:

  NS_BEGIN

  class Widget {
  PUBLIC
      void draw();
      void resize();
  PRIVATE
      int width;
      int height;
  };

  NS_END
  ```

- [x] Coroutine bodies — the written block folds exactly once and the coroutine transformation wrapper adds no duplicate fold; a coroutine lambda keeps its body fold

  ```cpp
  namespace std {

  template <typename Ret, typename...>
  struct coroutine_traits {
      using promise_type = typename Ret::promise_type;
  };

  template <typename = void>
  struct coroutine_handle {
      coroutine_handle() = default;

      template <typename Promise>
      coroutine_handle(coroutine_handle<Promise>) noexcept;

      static coroutine_handle from_address(void*) noexcept;
  };

  struct suspend_never {
      bool await_ready() const noexcept;
      void await_suspend(coroutine_handle<>) const noexcept;
      void await_resume() const noexcept;
  };

  }  // namespace std

  struct Task {
      struct promise_type {
          Task get_return_object();
          std::suspend_never initial_suspend();
          std::suspend_never final_suspend() noexcept;
          void return_void();
          void unhandled_exception();
      };
  };

  Task work() {
      int steps = 0;
      if (steps == 0) {
          steps += 1;
      }
      co_return;
  }

  void host() {
      auto nested = []() -> Task {
          int steps = 0;
          steps += 1;
          co_return;
      };
  }
  ```

- [x] Initializer-list constructions — the constructor's braces and the nested initializer list share delimiters and fold once; a parenthesized list argument keeps both folds

  ```cpp
  namespace std {

  template <typename T>
  class initializer_list {
  public:
      using size_type = decltype(sizeof(0));

      const T* ptr = nullptr;
      size_type len = 0;
  };

  }  // namespace std

  struct Bag {
      Bag(std::initializer_list<int> values);
  };

  Bag braces{
      1,
      2
  };

  Bag nested({
      3,
      4
  });
  ```

<!-- END GENERATED ITEMS -->

## Refinements

<!-- BEGIN GENERATED ITEMS: Refinements -->

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

- [x] Single-line constructs stay unfolded — a fold that hides nothing is noise

  ```cpp
  namespace tiny { }

  struct Empty {};

  enum Flags { A, B };

  void noop() {}

  int values[] = {1, 2, 3};

  auto lambda = [](int x) { return x; };

  int result = lambda(42);
  ```

<!-- END GENERATED ITEMS -->

## Changelog

| Date       | Change                                                                                                                     | PR                                                 |
| ---------- | -------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| 2026-08-01 | Nested compound statements, abbreviated function templates and coroutine bodies; instantiation dedup; semantics-table walk | [#568](https://github.com/clice-io/clice/pull/568) |
| —          | Block folding, list folding, access specifiers, preprocessor regions                                                       | —                                                  |
