# Folding Ranges

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

## Fold Kinds

<!-- BEGIN GENERATED ITEMS: fold_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Block folding**

functions, classes, structs, unions, enums, namespaces, lambdas

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "4:19-33:1", kind: namespace, collapsed_text: "{...}" }
  - { range: "6:17-10:1", kind: enum, collapsed_text: "{...}" }
  - { range: "12:13-15:1", kind: struct, collapsed_text: "{...}" }
  - { range: "17:12-20:1", kind: union, collapsed_text: "{...}" }
  - { range: "22:13-31:1", kind: class, collapsed_text: "{...}" }
  - { range: "25:15-30:5", kind: functionBody, collapsed_text: "{...}" }
  - { range: "26:36-28:9", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "36:0-42:1", kind: namespace, collapsed_text: "{...}" }
  - { range: "38:19-40:1", kind: struct, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested compound-statement folding**

`if`/`for`/`while` bodies inside functions

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "4:24-20:1", kind: functionBody, collapsed_text: "{...}" }
  - { range: "5:19-9:5", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "6:43-8:9", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "11:22-13:5", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "16:4-19:5", kind: compoundStmt, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Multi-line list folding**

Function parameters, call arguments, initializer lists, lambda captures

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "4:14-8:1", kind: functionParams, collapsed_text: "(...)" }
  - { range: "12:12-40:1", kind: functionBody, collapsed_text: "{...}" }
  - { range: "13:19-17:5", kind: initializer, collapsed_text: "{...}" }
  - { range: "19:24-23:5", kind: functionCall, collapsed_text: "(...)" }
  - { range: "25:15-28:5", kind: lambdaCapture, collapsed_text: "[...]" }
  - { range: "28:6-30:5", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "32:19-35:5", kind: functionParams, collapsed_text: "(...)" }
  - { range: "35:6-37:5", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "42:14-46:1", kind: functionParams, collapsed_text: "(...)" }
  - { range: "46:2-48:1", kind: functionBody, collapsed_text: "{...}" }
  - { range: "50:12-53:1", kind: functionParams, collapsed_text: "(...)" }
  - { range: "55:12-57:1", kind: struct, collapsed_text: "{...}" }
  - { range: "59:9-62:1", kind: functionCall, collapsed_text: "(...)" }
  - { range: "64:15-67:1", kind: initializer, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**Access-specifier section folding**

`public:` / `protected:` / `private:` regions within a class

```snap-folding_range
feature: folding_range
code: |
  class Widget {
  public:            // ┐
      void draw();   // │ foldable
      void resize(); // ┘
  private:           // ┐
      int width;     // │ foldable
      int height;    // ┘
  };
snapshot: |
  - { range: "5:13-12:1", kind: class, collapsed_text: "{...}" }
  - { range: "6:6-9:7", kind: accessSpecifier }
  - { range: "9:7-12:1", kind: accessSpecifier }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1661 clangd#2059 -->

**Preprocessor conditional folding (`#if` / `#ifdef` / `#ifndef` ... `#endif`)**

Branch regions delimited by `#else` fold today; a bare `#if ... #endif`
block without an `#else` does not fold yet. clangd#2059 is a duplicate
of clangd#1661.

```snap-folding_range
feature: folding_range
code: |
  #ifdef ENABLE_LOGGING    // ┐
  void log_message();      // │ no fold yet: bare conditional without #else
  #endif                   // ┘

  #ifdef USE_THREADS       // ┐
  void spawn_workers();    // │ folds: branches delimited by #else
  #else                    // │
  void run_inline();       // │
  #endif                   // ┘

  #ifdef USE_EPOLL         // ┐
  void poll_epoll();       // │ no fold yet: the branch before #elifdef
  #elifdef USE_KQUEUE      // │ ┐
  void poll_kqueue();      // │ │ folds: the #elifdef branch, delimited by #else
  #else                    // │ ┘
  void poll_select();      // │
  #endif                   // ┘
snapshot: |
  - { range: "14:7-16:5", kind: conditionDirective }
  - { range: "22:9-24:5", kind: conditionDirective }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**Custom region folding (`#pragma region` / `#pragma endregion`)**

```snap-folding_range
feature: folding_range
code: |
  #pragma region Configuration

  int retry_count = 3;
  int timeout_ms = 5000;

  #pragma endregion
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pragma classification**

Only the first argument token decides region/endregion

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "10:0-14:1", kind: region }
  - { range: "19:10-21:1", kind: region }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Comment folding**

multi-line `/* */` and consecutive `//` line comments

```snap-folding_range
feature: folding_range
code: |
  // This is a long
  // multi-line comment
  // that should fold as one region

  /*
   * Block comment
   * should also fold
   */
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Include region folding**

Consecutive `#include` directives

```snap-folding_range
feature: folding_range
code: |
  #include <vector>       // ┐
  #include <string>       // │ foldable region
  #include <algorithm>    // ┘

  #include "app.h"        // ┐ separate region
  #include "config.h"     // ┘ (blank line separates)
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Raw string literal folding**

```snap-folding_range
feature: folding_range
code: |
  auto sql = R"(
      SELECT *
      FROM users
      WHERE active = true
  )";  // foldable multi-line raw string
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`using` declaration blocks**

Consecutive using declarations/directives

```snap-folding_range
feature: folding_range
code: |
  using std::vector;  // ┐
  using std::string;  // │ foldable
  using std::map;     // ┘
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Template parameter list folding**

```snap-folding_range
feature: folding_range
code: |
  template<typename T>
  struct Less;

  template<
      typename Key,                 // ┐
      typename Value,               // │ foldable
      typename Compare = Less<Key>  // ┘
  >
  class SortedMap { };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template specializations and instantiations**

Written specializations and their members fold; instantiated declarations reuse the pattern's source locations and must not fold it again

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "5:11-11:1", kind: struct, collapsed_text: "{...}" }
  - { range: "8:17-10:5", kind: functionBody, collapsed_text: "{...}" }
  - { range: "14:17-18:1", kind: struct, collapsed_text: "{...}" }
  - { range: "15:17-17:5", kind: functionBody, collapsed_text: "{...}" }
  - { range: "21:15-23:1", kind: struct, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Abbreviated function templates**

Bodies of functions with `auto` or constrained `auto` parameters fold like any other function

```snap-folding_range
feature: folding_range
code: |
  template <typename T>
  concept Small = sizeof(T) <= 8;

  void consume(Small auto x) {
      auto copy = x;
      copy += 1;
  }

  void forward(auto value) {
      consume(value);
  }
snapshot: |
  - { range: "7:27-10:1", kind: functionBody, collapsed_text: "{...}" }
  - { range: "12:25-14:1", kind: functionBody, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-generated folding**

Braces and access specifiers spelled through macros fold at the invocation site

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "9:0-20:6", kind: namespace, collapsed_text: "{...}" }
  - { range: "11:13-18:1", kind: class, collapsed_text: "{...}" }
  - { range: "12:0-15:7", kind: accessSpecifier }
  - { range: "15:0-18:1", kind: accessSpecifier }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Coroutine bodies**

The written block folds exactly once and the coroutine transformation wrapper adds no duplicate fold; a coroutine lambda keeps its body fold

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "4:14-27:1", kind: namespace, collapsed_text: "{...}" }
  - { range: "7:24-9:1", kind: struct, collapsed_text: "{...}" }
  - { range: "12:24-19:1", kind: struct, collapsed_text: "{...}" }
  - { range: "21:21-25:1", kind: struct, collapsed_text: "{...}" }
  - { range: "29:12-37:1", kind: struct, collapsed_text: "{...}" }
  - { range: "30:24-36:5", kind: struct, collapsed_text: "{...}" }
  - { range: "39:12-45:1", kind: functionBody, collapsed_text: "{...}" }
  - { range: "41:20-43:5", kind: compoundStmt, collapsed_text: "{...}" }
  - { range: "47:12-53:1", kind: functionBody, collapsed_text: "{...}" }
  - { range: "48:31-52:5", kind: compoundStmt, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Initializer-list constructions**

The constructor's braces and the nested initializer list share delimiters and fold once; a parenthesized list argument keeps both folds

```snap-folding_range
feature: folding_range
code: |
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
snapshot: |
  - { range: "4:14-15:1", kind: namespace, collapsed_text: "{...}" }
  - { range: "7:23-13:1", kind: class, collapsed_text: "{...}" }
  - { range: "8:6-13:1", kind: accessSpecifier }
  - { range: "17:11-19:1", kind: struct, collapsed_text: "{...}" }
  - { range: "21:10-24:1", kind: initializer, collapsed_text: "{...}" }
  - { range: "26:10-29:2", kind: functionCall, collapsed_text: "(...)" }
  - { range: "26:11-29:1", kind: initializer, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Refinements

<!-- BEGIN GENERATED ITEMS: refinements -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**`collapsedText` placeholder (LSP 3.17)**

Show a summary when folded

> **Client support**: VS Code does **not** support `collapsedText` yet
> ([vscode#70794](https://github.com/microsoft/vscode/issues/70794) — still
> open); Neovim with nvim-lsp supports it natively. Clients that do not
> implement this field will silently ignore it — the folding still works,
> only the placeholder text is missing.

```snap-folding_range
feature: folding_range
code: |
  struct Config {
      int width;
      int height;
  };

  // When folded, the body collapses to a `{...}` placeholder while the
  // signature stays visible: int process_data(const Config& cfg) {...}
  int process_data(const Config& cfg) {
      return cfg.width * cfg.height;
  }
snapshot: |
  - { range: "11:14-14:1", kind: struct, collapsed_text: "{...}" }
  - { range: "18:36-20:1", kind: functionBody, collapsed_text: "{...}" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2666 -->

**Fold from the declaration line for function/class bodies**

Keep the signature visible when folded

> **Client support**: this depends on the client interpreting
> `FoldingRange.startLine` correctly. VS Code uses the line _after_
> `startLine` as the first hidden line, so setting `startLine` to the
> declaration line achieves the desired effect. However, VS Code still
> leaves the closing `}` on a separate line rather than collapsing it onto
> the signature line ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)
> — still open). Other clients may differ.

```snap-folding_range
feature: folding_range
code: |
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

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Inactive preprocessor branch indication**

Visually distinguish or auto-fold inactive `#if`/`#else` branches

The server emits a fold range for the region between the condition and
`#else`, so the first branch can be folded manually; the post-`#else`
branch gets no range yet. Knowing which branch is _inactive_ — to dim or
auto-fold it — is not implemented here; that information belongs to the
inactive-regions feature.

> **Note**: this overlaps with semantic tokens (inactive code dimming) and
> is partly a client UX concern. The server can mark these ranges with
> `FoldingRangeKind.Region` and clients can choose to auto-fold them.

```snap-folding_range
feature: folding_range
code: |
  #ifdef _WIN32
      // ... Windows code (active) ...
  #else
      // ... POSIX code (inactive, could auto-fold) ...
  #endif
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Single-line constructs stay unfolded**

A fold that hides nothing is noise

```snap-folding_range
feature: folding_range
code: |
  namespace tiny { }

  struct Empty {};

  enum Flags { A, B };

  void noop() {}

  int values[] = {1, 2, 3};

  auto lambda = [](int x) { return x; };

  int result = lambda(42);
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
