# 折叠范围

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

## 折叠类型

<!-- BEGIN GENERATED ITEMS: Fold Kinds -->

- [x] 块折叠 — 函数、类、结构体、联合体、枚举、命名空间、lambda

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 嵌套复合语句折叠 — 函数内 `if`/`for`/`while` 体

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 多行列表折叠 — 函数参数、调用参数、初始化列表、lambda 捕获列表

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 访问修饰符区域折叠 — 类内的 `public:` / `protected:` / `private:` 区域（[clangd#1455](https://github.com/clangd/clangd/issues/1455)）

  <details>
  <summary>示例</summary>

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

  </details>

- [ ] 预处理条件折叠（`#if` / `#ifdef` / `#ifndef` ... `#endif`）（部分实现）（[clangd#1661](https://github.com/clangd/clangd/issues/1661), [clangd#2059](https://github.com/clangd/clangd/issues/2059)）

  由 `#else` 分隔的分支区域现在可以折叠；没有 `#else` 的裸 `#if ... #endif`
  块尚不能折叠。clangd#2059 是 clangd#1661 的重复。

  <details>
  <summary>示例</summary>

  ```cpp
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
  ```

  </details>

- [x] 自定义区域折叠（`#pragma region` / `#pragma endregion`）（[clangd#1623](https://github.com/clangd/clangd/issues/1623)）

  <details>
  <summary>示例</summary>

  ```cpp
  #pragma region Configuration

  int retry_count = 3;
  int timeout_ms = 5000;

  #pragma endregion
  ```

  </details>

- [x] Pragma 分类 — 仅第一个参数 token 决定 region/endregion

  <details>
  <summary>示例</summary>

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

  </details>

- [ ] 注释折叠 — 多行 `/* */` 和连续的 `//` 行注释

  <details>
  <summary>示例</summary>

  ```cpp
  // This is a long
  // multi-line comment
  // that should fold as one region

  /*
   * Block comment
   * should also fold
   */
  ```

  </details>

- [ ] Include 区域折叠 — 连续的 `#include` 指令

  <details>
  <summary>示例</summary>

  ```cpp
  #include <vector>       // ┐
  #include <string>       // │ foldable region
  #include <algorithm>    // ┘

  #include "app.h"        // ┐ separate region
  #include "config.h"     // ┘ (blank line separates)
  ```

  </details>

- [ ] 原始字符串字面量折叠

  <details>
  <summary>示例</summary>

  ```cpp
  auto sql = R"(
      SELECT *
      FROM users
      WHERE active = true
  )";  // foldable multi-line raw string
  ```

  </details>

- [ ] `using` 声明块 — 连续的 using 声明/指令

  <details>
  <summary>示例</summary>

  ```cpp
  using std::vector;  // ┐
  using std::string;  // │ foldable
  using std::map;     // ┘
  ```

  </details>

- [ ] 模板参数列表折叠

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 模板特化和实例化 — 手写的特化及其成员折叠；实例化声明复用模板的源码位置，不得再次折叠

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 缩写函数模板 — 带有 `auto` 或受约束 `auto` 参数的函数体与其他函数一样折叠

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 宏生成的折叠 — 通过宏拼写出来的花括号和访问修饰符在调用处折叠

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 协程体 — 原始代码块恰好折叠一次，协程转换包装不会产生重复折叠；协程 lambda 的函数体仍可折叠

  <details>
  <summary>示例</summary>

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

  </details>

- [x] 初始化列表构造 — 构造函数的大括号与嵌套初始化列表共用分隔符，只折叠一次；圆括号包裹的列表参数保留两处折叠

  <details>
  <summary>示例</summary>

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

  </details>

<!-- END GENERATED ITEMS -->

## 改进

<!-- BEGIN GENERATED ITEMS: Refinements -->

- [x] `collapsedText` 占位文本（LSP 3.17）— 折叠后显示摘要（[clangd#2667](https://github.com/clangd/clangd/issues/2667)）

  > **客户端支持**：VS Code **不支持** `collapsedText`（[vscode#70794](https://github.com/microsoft/vscode/issues/70794) — 仍为
  > open）；Neovim 的 nvim-lsp 原生支持。不支持此字段的客户端会静默忽略 —
  > 折叠仍然有效，只是缺少占位文本。

  <details>
  <summary>示例</summary>

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

  </details>

- [ ] 从声明行开始折叠函数/类体 — 折叠后保留签名可见（[clangd#2666](https://github.com/clangd/clangd/issues/2666)）

  > **客户端支持**：这取决于客户端是否正确解释
  > `FoldingRange.startLine`。VS Code 将 `startLine` _之后_
  > 的行作为第一个隐藏行，因此将 `startLine` 设为声明行即可达到预期效果。
  > 但 VS Code 仍会将右花括号 `}` 单独留在下一行，而不是将其折叠到
  > 签名行（[vscode#3352](https://github.com/microsoft/vscode/issues/3352)
  > — 仍为 open）。其他客户端可能不同。

  <details>
  <summary>示例</summary>

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

  </details>

- [ ] 非活跃预处理分支指示 — 视觉区分或自动折叠非活跃的 `#if`/`#else` 分支 _(部分实现)_

  服务器为条件和 `#else` 之间的区域发出折叠范围，因此第一个分支可以手动折叠；`#else` 之后的分支还没有范围。知道哪个分支是 _非活跃_ 的——以便灰显或自动折叠它——这里没有实现；该信息属于非活跃区域特性。

  > **备注**：这与 semantic tokens（非活跃代码灰显）重叠，并且部分是客户端 UX 范畴的问题。服务器可以用 `FoldingRangeKind.Region` 标记这些范围，客户端可以选择自动折叠它们。

  <details>
  <summary>示例</summary>

  ```cpp
  #ifdef _WIN32
      // ... Windows code (active) ...
  #else
      // ... POSIX code (inactive, could auto-fold) ...
  #endif
  ```

  </details>

- [x] 单行结构保持不折叠——隐藏不了任何内容的折叠是噪音

  <details>
  <summary>示例</summary>

  ```cpp
  namespace tiny { }

  struct Empty {};

  enum Flags { A, B };

  void noop() {}

  int values[] = {1, 2, 3};

  auto lambda = [](int x) { return x; };

  int result = lambda(42);
  ```

  </details>

<!-- END GENERATED ITEMS -->
