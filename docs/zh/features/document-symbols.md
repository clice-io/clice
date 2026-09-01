# 文档符号

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

通过 `textDocument/documentSymbol` 提供文件大纲和面包屑导航：一个嵌套符号树，包含范围、选择范围以及用于区分重载并显示声明类型的 `detail` 字段。

## 符号层级

<!-- BEGIN GENERATED ITEMS: Symbol Hierarchy -->

- [x] 嵌套符号树 — 符号按书写作用域嵌套；类外定义出现在其词法位置并使用限定名

  <details>
  <summary>示例</summary>

  ```cpp
  namespace demo {

  struct Point {
      int x;
      int y;

      int manhattan() const;
  };

  int Point::manhattan() const {
      return x + y;
  }

  enum class Axis { X, Y };

  int origin_distance(const Point& p);

  namespace inner {
  constexpr int level = 2;
  }

  }  // namespace demo

  // A reopened namespace gets its own outline node per written scope.
  namespace demo {
  int reopened();
  }

  namespace demo::nested {
  int compact();
  }
  ```

  </details>

- [x] 符号范围和选择范围 — 范围覆盖整个声明；选择范围覆盖完整书写名称，包括 `~Widget`、`operator==` 和 `operator bool` 这样的多 token 名称

  <details>
  <summary>示例</summary>

  ```cpp
  namespace members {

  struct Widget {
      Widget();
      explicit Widget(int size);
      ~Widget();

      Widget& operator=(const Widget& other);
      bool operator==(const Widget& other) const;
      operator bool() const;

      static int instances();

      int size;
      unsigned bits : 3;
      const char* name = "widget";
  };

  Widget::Widget(int size) : size(size), bits(0) {}

  int Widget::instances() {
      return 0;
  }

  }  // namespace members
  ```

  </details>

- [ ] 访问修饰符分组 — 将 `public:` / `private:` / `protected:` 作为分组节点用于面包屑导航（[clangd#499](https://github.com/clangd/clangd/issues/499)）

  <details>
  <summary>示例</summary>

  ```cpp
  class Widget {
  public:
      void draw();
      void resize();

  private:
      int width;
      int height;
  };
  ```

  </details>

- [x] 匿名作用域和 inline 作用域 — 匿名命名空间、无名结构体和 union 将其成员归入占位名称下；inline namespace 成员保留在 inline namespace 节点下

  <details>
  <summary>示例</summary>

  ```cpp
  namespace {

  int hidden_counter = 0;

  }  // namespace

  namespace misc {

  inline namespace v1 {

  int versioned();

  }  // namespace v1

  struct Outer {
      struct {
          int anonymous_member;
      };

      union {
          int as_int;
          float as_float;
      };
  };

  }  // namespace misc
  ```

  </details>

- [x] UTF-16 位置编码 — 非 ASCII 文本之后的列按 UTF-16 代码单元计数

  <details>
  <summary>示例</summary>

  ```cpp
  // π ≈ 3.14159, 中文注释
  constexpr double 半径 = 2.0;
  constexpr double π值 = 3.14159; double area();
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 符号种类

<!-- BEGIN GENERATED ITEMS: Symbol Kinds -->

- [x] 核心符号种类 — 命名空间、类、结构体、联合体、枚举及其成员、函数、变量、字段、结构化绑定和 lambda 都会出现在大纲中，并映射到对应的 LSP 符号种类

  <details>
  <summary>示例</summary>

  ```cpp
  namespace kinds {

  union Value {
      int i;
      float f;
  };

  enum Flags { FlagA, FlagB };

  enum class Mode : unsigned char { Fast, Safe };

  struct Pair {
      struct Meta {
          int tag;
      };

      int first;
      int second;
      static int instances;
  };

  Pair make_pair();

  auto [bound_first, bound_second] = make_pair();

  auto lambda = [](int x) {
      return x * 2;
  };

  }  // namespace kinds
  ```

  </details>

- [x] 模板声明 — 类模板、函数模板和变量模板带有 `template ` detail 前缀；concept 和缩写函数模板（`concept auto` 参数）也会出现

  <details>
  <summary>示例</summary>

  ```cpp
  namespace templates {

  template <typename T>
  struct Box {
      T value;

      void reset();
  };

  template <typename T>
  void Box<T>::reset() {}

  template <typename T>
  T zero() {
      return T();
  }

  template <typename T>
  constexpr T pi = T(3.14159);

  template <typename T>
  concept Small = sizeof(T) <= 4;

  void takes_concept(Small auto x);

  }  // namespace templates
  ```

  </details>

- [x] 模板特化和推导指南 — 类和变量模板的显式与部分特化在名称中带模板实参出现；成员嵌套在对应特化下；推导指南渲染其推导出的签名

  <details>
  <summary>示例</summary>

  ```cpp
  namespace spec {

  template <typename T>
  struct Box {
      T value;
  };

  template <>
  struct Box<void> {};

  template <typename T>
  struct Box<T*> {
      T* pointee;
  };

  template <typename T>
  T zero() {
      return T();
  }

  template <>
  int zero<int>();

  template <typename T>
  constexpr T pi = T(3);

  template <>
  constexpr int pi<int> = 3;

  template <typename T>
  constexpr T* pi<T*> = nullptr;

  template <typename T>
  struct Deduced {
      Deduced(T raw);
  };

  template <typename T>
  Deduced(T*) -> Deduced<T>;

  // Forces the implicit instantiation Box<int>, which must not appear.
  Box<int> instantiated;

  // An explicit class instantiation gets a childless node; the instantiated
  // members and the function instantiation (whose location clang records at
  // the primary) produce no symbols.
  template struct Box<char>;
  template long zero<long>();

  }  // namespace spec
  ```

  </details>

- [x] 类型别名 — `typedef`、`using` 别名和别名模板以 `type alias` detail 出现在大纲中

  <details>
  <summary>示例</summary>

  ```cpp
  namespace aliases {

  struct Widget {};

  typedef Widget LegacyWidget;

  using ModernWidget = Widget;

  template <typename T>
  struct Box {};

  template <typename T>
  using BoxOf = Box<T>;

  struct Holder {
      using Inner = Widget;
  };

  }  // namespace aliases
  ```

  </details>

- [ ] 显式实例化指令 — 类形式显示为无子节点符号；clang 将函数和变量形式定位到模式处，因此它们在大纲中缺失 _(部分实现)_ ([llvm#191658](https://github.com/llvm/llvm-project/issues/191658))

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Box {
      T value;
  };

  template struct Box<int>;
  extern template struct Box<char>;

  template <typename T>
  void convert(T value) {}

  template void convert<int>(int);

  template <typename T>
  T zero = T();

  template int zero<int>;
  ```

  </details>

- [x] 宏定义 — 对象式宏和函数式宏定义出现在大纲中，参数列表作为函数式宏的 detail ([clangd#1744](https://github.com/clangd/clangd/issues/1744))

  <details>
  <summary>示例</summary>

  ```cpp
  // The assertion holds the directives out of the preamble region, whose
  // live record the server path does not yet see.
  static_assert(true);

  #define MAX_BUFFER_SIZE 4096
  #define CHECK(cond, msg) ((cond) ? 0 : (msg))
  #define TRACE(...) log(__VA_ARGS__)
  #define SPLIT_\
  LIMIT 7

  struct Config {
  #define CONFIG_VERSION 3
      int version = CONFIG_VERSION;
  };
  ```

  </details>

- [ ] preamble 区域中的宏 — 前导指令序列中的定义在 inspect 路径上会产生大纲，而服务器的 preamble 记录尚未呈现它们 _(部分实现)_

  <details>
  <summary>示例</summary>

  ```cpp
  #define PREAMBLE_LIMIT 8
  #define PREAMBLE_CHECK(cond) (!!(cond))

  int after = PREAMBLE_LIMIT;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 符号详情

<!-- BEGIN GENERATED ITEMS: Symbol Detail -->

- [x] 函数签名 — `detail` 字段中的参数和返回类型用于重载区分；构造函数省略 `void` 返回类型 ([clangd#520](https://github.com/clangd/clangd/issues/520), [clangd#601](https://github.com/clangd/clangd/issues/601), [clangd#1232](https://github.com/clangd/clangd/issues/1232))

  <details>
  <summary>示例</summary>

  ```cpp
  namespace detail {

  void process(int x);
  void process(const char* s);

  struct Task {
      Task();
      Task(int priority);

      int run(bool async) const;
  };

  }  // namespace detail
  ```

  </details>

- [x] 变量和字段类型 — `detail` 字段中的声明类型；lambda 渲染为 `(lambda)`

  <details>
  <summary>示例</summary>

  ```cpp
  namespace detail {

  int timeout = 30;
  const char* logger_name = "core";

  struct Config {
      unsigned retries;
      double backoff;
  };

  auto on_error = [](int code) {
      return code != 0;
  };

  }  // namespace detail
  ```

  </details>

- [x] 默认参数剥离 — 签名从函数类型推导得出，因此默认参数值绝不会泄漏到大纲中 ([clangd#221](https://github.com/clangd/clangd/issues/221))

  <details>
  <summary>示例</summary>

  ```cpp
  namespace detail {

  void open_file(const char* path, int mode = 0644);

  struct Server {
      void listen(int port = 8080, int backlog = 128);
  };

  }  // namespace detail
  ```

  </details>

- [ ] 基类在 detail 中 — 在派生类声明上显示 `: Shape`

  <details>
  <summary>示例</summary>

  ```cpp
  struct Shape {};

  struct Circle : Shape {
      double radius;
  };
  ```

  </details>

- [x] 多行签名范围 — 符号范围从声明开头开始并跨越完整签名，因此编辑器 sticky scroll 能正确锚定 ([clangd#2221](https://github.com/clangd/clangd/issues/2221))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Config {};

  void process_data(
      const Config& cfg,
      int flags
  ) {}
  ```

  </details>

- [x] 作用域类型 — detail 中写出的类作用域恰好出现一次，无论嵌套类、模板 ID、别名还是依赖名称都是如此

  <details>
  <summary>示例</summary>

  ```cpp
  namespace scoped {

  struct Outer {
      struct Inner {};
      template <typename T> struct Box {};
      using Alias = int;
  };

  struct User {
      Outer::Inner plain;
      Outer::Box<int> boxed;
      Outer::Alias aliased;
      const Outer::Inner frozen;
  };

  template <typename T>
  struct Holder {
      typename T::type value;
      typename T::inner::type deep;
      typename T::template rebind<int> bound;
  };

  }  // namespace scoped
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 缺失的符号

<!-- BEGIN GENERATED ITEMS: Missing Symbols -->

- [ ] include 指令 — 大纲中的 `#include` 条目（[clangd#2226](https://github.com/clangd/clangd/issues/2226)）

  <details>
  <summary>示例</summary>

  ```cpp
  #include "config.h"

  int uses_config();
  ```

  </details>

- [x] 局部符号 — 函数体内声明的变量和类型嵌套在其函数下方（[clangd#616](https://github.com/clangd/clangd/issues/616)）

  <details>
  <summary>示例</summary>

  ```cpp
  int compute() {
      int local_sum = 0;

      struct Accumulator {
          int total;
      };

      auto twice = [](int x) {
          return 2 * x;
      };

      struct Pair {
          int a;
          int b;
      };

      auto [first, second] = Pair{1, 2};

      return local_sum + twice(first) + second;
  }
  ```

  </details>

- [ ] 模块声明 — 大纲中的 `export module`、`module` 和 `import` 声明

  <details>
  <summary>示例</summary>

  ```cpp
  export module app.core;

  import std;

  export int core_entry();
  ```

  </details>

- [ ] `#pragma mark` 导航标记 — 作为大纲条目的编辑器分段标记

  <details>
  <summary>示例</summary>

  ```cpp
  #pragma mark - Lifecycle

  void setup();

  #pragma mark - Rendering

  void draw();
  ```

  </details>

- [x] 友元函数定义 — 在类内联定义的友元函数出现在该类下方

  <details>
  <summary>示例</summary>

  ```cpp
  struct Owner {
      friend void inline_friend(Owner& o) {}

      friend bool operator==(const Owner& lhs, const Owner& rhs) {
          return &lhs == &rhs;
      }
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 符号标签

<!-- BEGIN GENERATED ITEMS: Symbol Tags -->

- [ ] Deprecated 标签 — 用 LSP `deprecated` 符号标签标记 `[[deprecated]]` 符号

  <details>
  <summary>示例</summary>

  ```cpp
  [[deprecated("use open_v2")]] void open_v1();

  void open_v2();
  ```

  </details>

- [ ] 访问和存储指示符 — 大纲条目上的 public / private / protected、static、virtual 和 abstract 标记（[clangd#2123](https://github.com/clangd/clangd/issues/2123)）

  <details>
  <summary>示例</summary>

  ```cpp
  class Base {
  public:
      virtual void render() = 0;

  protected:
      static int instances();

  private:
      int id;
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 位置正确性

<!-- BEGIN GENERATED ITEMS: Location Correctness -->

- [x] 宏展开产生的符号 — 宏调用生成的符号定位在调用处，而不是宏定义处（[clangd#475](https://github.com/clangd/clangd/issues/475)）

  <details>
  <summary>示例</summary>

  ```cpp
  // The assertion holds the directives out of the preamble region, whose
  // live record the server path does not yet see.
  static_assert(true);

  #define DEFINE_HANDLER(name) void name()

  DEFINE_HANDLER(on_ready);
  DEFINE_HANDLER(on_close);

  #define DECLARE_CLASS(X) class X
  DECLARE_CLASS(Generated) {
      int member;
  };
  ```

  </details>

- [x] 宏参数中拼写的名称 — 选择范围指向宏参数中写出的名称；宏体里拼写的名称回退到调用位置（[clangd#1941](https://github.com/clangd/clangd/issues/1941)）

  <details>
  <summary>示例</summary>

  ```cpp
  // The assertion holds the directives out of the preamble region, whose
  // live record the server path does not yet see.
  static_assert(true);

  #define VAR(X) int X = 1;

  VAR(from_argument)

  #define COUNTER() int counter_from_body = 0;

  COUNTER()
  ```

  </details>

<!-- END GENERATED ITEMS -->
