# 悬停

光标下符号的富信息卡片。

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## 符号信息

<!-- BEGIN GENERATED ITEMS: Symbol Information -->

- [x] 限定名 — 悬停卡片显示所属命名空间和类作用域

  <details>
  <summary>示例</summary>

  ```cpp
  namespace app::detail {

  struct Engine {
      void tick() {
          int count = 0;
      }
  };

  int workers = 4;

  }

  int global = 1;
  ```

  </details>

- [x] 符号种类 — 卡片说明符号是什么：struct、enum、function、field 等

  <details>
  <summary>示例</summary>

  ```cpp
  namespace kinds {

  struct Point {
      int x;
  };

  union Packet {
      int raw;
  };

  enum class Color {
      Red,
  };

  using Alias = Point;

  int length(Point p) {
      return p.x;
  }

  }
  ```

  </details>

- [x] 访问说明符 — 成员显示其 public / protected / private 访问级别

  <details>
  <summary>示例</summary>

  ```cpp
  class Account {
  public:
      int balance;

  protected:
      int limit;

  private:
      int pin;
  };
  ```

  </details>

- [x] 定义渲染 — 卡片包含符号的源码定义

  <details>
  <summary>示例</summary>

  ```cpp
  namespace retry {

  constexpr int max_retries = 3;

  int backoff(int attempt = 1) {
      return attempt * max_retries;
  }

  }
  ```

  </details>

- [ ] 初始化器截断 — 大型初始化器截断显示，而不是完整显示 _(部分)_ ([clangd#710](https://github.com/clangd/clangd/issues/710))

  渲染出的定义省略了初始化器，但求值后的 `Value` 字段仍然列出全部 256 个元素。

  <details>
  <summary>示例</summary>

  ```cpp
  #define A(x) x, x, x, x
  #define B(x) A(A(A(A(x))))
  int arr[] = {B(0)};
  ```

  </details>

- [ ] 虚修饰符 — 方法悬停显示 `virtual` / `override` / `final` _(部分)_ ([clangd#2474](https://github.com/clangd/clangd/issues/2474))

  源码中写出的修饰符会渲染出来（`virtual … = 0`、`override`、`final`），但重写方法如果省略了冗余的 `virtual` 关键字，就看不到虚函数的迹象——卡片缺少 issue 要求的 `virtual void draw() override` 形式。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Base {
      virtual void draw() = 0;
  };

  struct Circle : Base {
      void draw() override;
  };

  struct Dot final : Circle {
      void draw() final;
  };
  ```

  </details>

- [ ] 匿名命名空间作用域 — 作用域显示中显示 `(anonymous namespace)` _(部分)_ ([clangd#436](https://github.com/clangd/clangd/issues/436))

  卡片会渲染，但匿名段从作用域显示中丢掉了：顶层匿名成员完全不显示作用域行，`outer::(anonymous)` 只显示 `outer`。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace {
  int hidden = 1;
  }

  namespace outer {
  namespace {
  int nested = 2;
  }
  }

  int sum = hidden + outer::nested;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 类型信息

<!-- BEGIN GENERATED ITEMS: Type Information -->

- [x] 变量类型 — 指针、引用、数组

  变量的卡片对声明类型进行美化打印，按照源码中的读法拼出指针、引用和数组声明符。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace variable_type {

  int target;

  int *ptr = &target;

  int &ref = target;

  int numbers[4]{};

  }
  ```

  </details>

- [x] 类型别名 — 脱糖后的 `aka` 形式

  带糖的类型把底层类型显示为 `Alias (aka int)`。`show_aka` 选项可以关掉 `aka` 后缀。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace aka_desugar {

  using Handle = int;
  using Alias = Handle;

  Handle direct = 0;

  Alias chained = 0;

  }
  ```

  </details>

- [x] 函数签名 — 返回类型、参数名、默认值

  函数卡片列出返回类型、每个参数及其名称，以及任何默认实参。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace function_signature {

  int add(int lhs, int rhs);

  void configure(int width, bool visible = true);

  }
  ```

  </details>

- [x] 模板参数 — 类型、模板模板、非类型

  每种模板参数都会报告其形式：类型参数、模板模板参数，以及带默认值的非类型参数。

  <details>
  <summary>示例</summary>

  ```cpp
  // Template type parameter.
  namespace type_param {
  template <typename T = int> void foo();
  }

  // Template template parameter.
  namespace template_template_param {
  template <template<typename> class T> void foo();
  }

  // Non-type template parameter.
  namespace non_type_param {
  template <int T = 5> void foo();
  }
  ```

  </details>

- [x] `auto` 推导 — 占位符解析成的类型

  悬停 `auto` 占位符会显示替换进去的类型——内置类型、指针、lambda、模板实例化，以及未实例化模板中的 `/* not deduced */` 标记。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace auto_deduction {

  struct Bar {};
  struct Pair { int first; int second; };
  template <typename T> struct Box {};

  void locals() {
    int n = 0;
    auto a = 1;
    const auto b = 1;
    auto& c = n;
    auto* d = &n;
    auto e = &n;
    auto f = []{};
    auto g = Box<int>();
    auto [x, y] = Pair{};
  }

  auto with_trailing() -> int { return 0; }

  auto deduced_return() { return Bar(); }

  template <typename T> void undeduced() {
    auto u = T();
  }

  }
  ```

  </details>

- [x] `decltype` 推导 — 值、引用和依赖形式

  悬停 `decltype` 或 `decltype(auto)` 占位符会显示解析出的类型，包括括号表达式规则添加的引用。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace decltype_deduction {

  int base = 0;

  void locals() {
    int n = 0;
    const int cn = 0;
    int& r = n;
    decltype(auto) a = 1;
    decltype(auto) b = cn;
    decltype(auto) c = r;
    decltype(n) d = n;
    decltype((n)) e = n;
    decltype(static_cast<int&&>(n)) f = static_cast<int&&>(n);
  }

  decltype(base) mirror = base;

  template <typename T> decltype(auto) undeduced() { return T(); }

  template <typename T> struct Dependent {
    using kind = decltype(T::member);
  };

  }
  ```

  </details>

- [ ] CTAD — 类占位符推导出的模板参数 _(部分)_ ([clangd#435](https://github.com/clangd/clangd/issues/435))

  使用类模板实参推导时，变量卡片会显示推导出的 `Box<int>`，但悬停类名拼写仍然报告不带实参的主模板。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace ctad_arguments {

  template <typename T> struct Box {
    Box(T);
  };

  Box picked(42);

  }
  ```

  </details>

- [ ] 实例化实参 — 模板参数在使用处绑定 _(部分)_ ([clangd#230](https://github.com/clangd/clangd/issues/230))

  模板使用处会显示替换后的类型（`Wrapper<int>`、`identity<int>`、`int x`），但不会显示每个参数绑定到哪个实参的显式 `T = int` 映射。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace instantiation_args {

  template <typename T> struct Wrapper {
    T value;
  };

  template <typename T> T identity(T x) {
    return x;
  }

  void demo() {
    Wrapper<int> holder;
    int r = identity(42);
  }

  }
  ```

  </details>

- [ ] Lambda `auto` 参数——推导出的参数类型 ([clangd#493](https://github.com/clangd/clangd/issues/493))

  悬停泛型 lambda 的 `auto` 参数不会产生卡片；
  推导出的参数类型不会显示。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace lambda_auto_params {

  auto printer = [](auto value) { return value; };

  }
  ```

  </details>

- [x] 糖化 `auto`——推导过程中保留别名糖

  clangd 将 `auto` 丢失别名糖的问题跟踪为 clangd#709；clice
  已保留别名拼写并附加其脱糖形式，因此
  从别名返回类型推导的 `auto` 显示为 `Outer // aka: int`。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace sugared_auto {

  using Inner = int;
  using Outer = Inner;

  Outer make();

  void demo() {
    auto value = make();
  }

  }
  ```

  </details>

- [ ] 类型格式化——对渲染的类型应用 clang-format ([clangd#2156](https://github.com/clangd/clangd/issues/2156))

  长或嵌套的类型由编译器的默认类型打印器打印；
  它们不会通过 clang-format 重新换行或对齐。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace clang_format_types {

  template <typename A, typename B, typename C, typename D>
  struct Tuple {};

  Tuple<int, long, unsigned, char> wide;

  }
  ```

  </details>

- [x] 匿名结构体 typedef——经典 C `typedef struct {…} Name` ([clangd#2219](https://github.com/clangd/clangd/issues/2219))

  按 C11 编译：clangd 对匿名结构体的别名渲染出误导性的 `struct Point`；
  clice 将该结构体以其 typedef 命名，
  因此别名及其变量都得到干净的 `Point` 卡片。

  <details>
  <summary>示例</summary>

  ```cpp
  /// A 2-D point.
  typedef struct {
    int x, y;
  } Point;

  Point origin = {.y = 2, .x = 1};
  ```

  </details>

- [ ] 概念约束——参数或 `auto` 占位符背后的约束 _(部分)_

  受约束参数和概念引用卡片带有约束，
  但悬停受约束的 `Addable auto` 变量的占位符时只显示推导类型——约束被丢弃了。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace concept_constraints {

  template <typename T>
  concept Addable = requires(T a) { a + a; };

  template <Addable U>
  void sum(U a, U b);

  auto flag = Addable<int>;

  Addable auto total = 1;

  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 布局信息

<!-- BEGIN GENERATED ITEMS: Layout Information -->

- [x] 字段布局——大小、偏移、对齐和填充在字段悬停时显示

  语料库固定为 x86-64 目标，因此位数是稳定的。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Header {
      char tag;
      int length;
  };

  struct Flags {
      int ready : 1;
      int end : 1;
  };
  ```

  </details>

- [ ] 类型级布局——悬停类型本身显示其大小、对齐和填充 _(部分)_ ([clangd#1763](https://github.com/clangd/clangd/issues/1763))

  大小和对齐目前显示在类型卡片上；
  总填充还没有显示。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace layout {

  struct Widget {
      int id;
      double value;
  };

  }
  ```

  </details>

- [ ] 虚函数表偏移 — 虚方法显示其表槽 _(partial)_ ([clangd#1771](https://github.com/clangd/clangd/issues/1771))

  目前方法卡片不渲染任何 vtable 信息。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Shape {
      virtual void draw();
      virtual void move();
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 表达式上下文

<!-- BEGIN GENERATED ITEMS: Expression Context -->

- [x] 常量求值 — constexpr、枚举器、sizeof

  当初始化器是常量表达式时，卡片会求值并显示结果值。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace constant_value {

  constexpr int square(int n) { return n * n; }
  int from_call = square(5);

  int from_sizeof = sizeof(int);

  enum Color { Red = -1, Green = 5 };
  Color picked = Green;

  template <int A, int B> struct Sum { static constexpr int value = A + B; };
  int from_member = Sum<3, 4>::value;

  }
  ```

  </details>

- [x] 调用实参 — 每个实参绑定到哪个形参

  在调用点悬停实参时，会显示其传入的形参，并标明其绑定的形参名。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace callee_arguments {

  void configure(int width, int& out, int flags = 0);

  void demo() {
    int w = 1024;
    int result = 0;
    configure(w, result, 3);
  }

  }
  ```

  </details>

- [x] 传递语义 — 按值、按引用、按 const 引用

  实参卡片说明值如何到达被调用者：按值复制，或绑定到可变或 const 引用形参。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace pass_semantics {

  void by_value(int x);
  void by_ref(int& x);
  void by_const_ref(const int& x);

  void demo() {
    int n = 0;
    by_value(n);
    by_ref(n);
    by_const_ref(n);
  }

  }
  ```

  </details>

- [x] 隐式转换 — 实参转换为形参类型

  当实参通过隐式转换传给形参时，卡片会标注目标类型，无论是内建转换还是用户自定义转换。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace implicit_conversion {

  struct Wrapper {
    Wrapper(int value);
  };

  void take_float(float x);
  void take_wrapper(Wrapper w);

  void demo() {
    int n = 0;
    take_float(n);
    take_wrapper(n);
  }

  }
  ```

  </details>

- [ ] 字符串字面量 — 悬停时报告长度 _(partial)_ ([clangd#1016](https://github.com/clangd/clangd/issues/1016))

  字符串字面量卡片报告数组类型及其字节大小（`const char[6]`、`Size: 6 bytes`——长度加上 null 终止符），而不是显式的字符数。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace string_length {

  const char *greeting = "hello";

  }
  ```

  </details>

- [ ] 数值字面量 — 整数或浮点字面量的类型和值 ([clangd#1669](https://github.com/clangd/clangd/issues/1669))

  悬停数值字面量不会产生卡片，而字符和字符串字面量的类型和值会显示。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace numeric_literal_type {

  auto count = 42;
  auto ratio = 3.14;

  }
  ```

  </details>

- [ ] Record 变量 — 外层常量值混入（部分）（[clangd#1622](https://github.com/clangd/clangd/issues/1622)）

  悬停一个可常量求值调用的 record 类型实参时，当前会把该调用的值（`Value = 7`）报告到该变量上——这个值并不是该 record 自身的值。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace record_value_misleading {

  struct Tag {};

  constexpr int rank(Tag) {
    return 7;
  }

  void demo() {
    Tag t;
    int r = rank(t);
  }

  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 文档

<!-- BEGIN GENERATED ITEMS: Documentation -->

- [x] Doxygen `///` 注释 — 从声明中提取，并在悬停时渲染

  适用于普通函数、主模板及其特化；引用会解析到最特化声明的注释。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace docs {
  /// Adds two integers.
  int add(int a, int b);

  /// A box holding a value.
  template <typename T> struct Box {};

  /// A box of pointers.
  template <typename T> struct Box<T*> {};

  void use() {
      Box<int> b;
      Box<int*> p;
  }
  }
  ```

  </details>

- [x] 合成访问器文档 — 简单 getter/setter 获得一行生成的描述

  自身没有注释的简单 getter 或 setter 会在其悬停卡片中生成一行合成的
  "Trivial accessor/setter for `field`." 文本。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace accessors {
  struct Widget {
      int width;
      int getWidth() { return width; }
      void setWidth(int w) { width = w; }
  };
  }
  ```

  </details>

- [ ] `@copydoc` 标签 — 将另一个符号的文档复制到当前符号上（部分）（[clangd#1320](https://github.com/clangd/clangd/issues/1320)）

  `@copydoc target` 标签应将 `target` 的文档复制到当前符号的悬停卡片中。clice 目前还不解析该标签——卡片显示的是字面文本 `@copydoc base_func()`。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace copydoc {
  /// Detailed documentation.
  void base_func();

  /// @copydoc base_func()
  void wrapper();
  }
  ```

  </details>

- [ ] 继承的重写方法文档 — 没有注释的 override 应显示基类方法的文档（部分）（[clangd#2504](https://github.com/clangd/clangd/issues/2504)）

  悬停一个自身没有注释的重写方法时，应显示它所重写方法的文档。clice 目前还不继承该文档——重写方法的卡片中没有任何描述。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace inherit_docs {
  struct Base {
      /// Renders the widget.
      virtual void draw();
  };
  struct Circle : Base {
      void draw() override;
  };
  }
  ```

  </details>

- [ ] 重载文档共享 — 后续没有注释的重载应复用第一个重载的文档（部分）（[clangd#2506](https://github.com/clangd/clangd/issues/2506)）

  连续的重载通常只给第一个写文档；后续未写文档的重载应复用这份共享描述。clice 目前还不共享该文档——后续重载的卡片中没有任何描述。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace overloads {
  /// Opens a file.
  void open(const char* path);
  void open(const char* path, int flags);
  }
  ```

  </details>

- [ ] 继承构造函数文档 — `using Base::Base;` 应显示基类构造函数的文档（[clangd#1936](https://github.com/clangd/clangd/issues/1936)）

  通过 `using Base::Base;` 引入的构造函数在悬停时应带有基类构造函数的文档。目前没有对应的悬停界面：using 声明中的名字解析到类，而不是继承的构造函数。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace inherited_ctor {
  struct Base {
      /// Constructs from a value.
      Base(int value);
  };
  struct Derived : Base {
      using Base::Base;
  };
  }
  ```

  </details>

- [ ] 横幅注释 — 与后文用空行分隔的段落横幅不应附着到下一个声明（部分）（[clangd#974](https://github.com/clangd/clangd/issues/974)）

  `// ==== Section ====` 这样的横幅后跟空行时，不应被误当作其下声明的文档。clice 目前仍会附着它——横幅文本会出现在卡片中。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace banners {
  // ==== Section Banner ====

  void foo();
  }
  ```

  </details>

- [x] 声明与定义注释 — 优先使用声明的文档，而不是定义处的注释

  clangd 将其跟踪为 clangd#829；clice 已经优先使用声明处的 `///` 文档，而不是定义处的普通 `//` 注释，并在声明处和定义处都显示它。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace decldef {
  /// Public API documentation.
  void process(int x);

  // Internal implementation note.
  void process(int x) { (void)x; }
  }
  ```

  </details>

- [ ] 空白和换行 — 注释中的 markdown 表格保留其换行（部分）（[clangd#2057](https://github.com/clangd/clangd/issues/2057)）

  跨多行 `///` 编写的 markdown 表格应按保留换行的方式渲染为表格。clice 目前将多行压成一行，因此表格无法渲染。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace tables {
  /// | Column A | Column B |
  /// |----------|----------|
  /// | 1        | 2        |
  void table_fn();
  }
  ```

  </details>

- [ ] 注释缩进 — 注释中缩进的文本行不应产生多余的额外缩进（部分）（[clangd#1040](https://github.com/clangd/clangd/issues/1040)）

  文档注释正文包含缩进代码块时，应以正确的缩进渲染。clice 目前会去掉前导缩进，导致缩进代码块丢失偏移量，空行也会塌陷。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace indented {
  /// Summary line.
  ///
  ///     step_one();
  ///     step_two();
  void run();
  }
  ```

  </details>

- [ ] 宏展开产生的 template 关键字 — 文档注释应在展开后保留 _(部分)_ ([clangd#1226](https://github.com/clangd/clangd/issues/1226))

  当 `template` 关键字来自宏展开时，声明的文档注释仍应出现在 hover 中。clice 目前会丢弃它——卡片没有描述。

  <details>
  <summary>示例</summary>

  ```cpp
  int anchor = 0;

  #define TEMPLATE template

  /// A documented template function.
  TEMPLATE <typename T> void run(T value);
  ```

  </details>

- [ ] 注释抑制选项 — 配置开关，用于隐藏错误归属的文档注释 ([clangd#2148](https://github.com/clangd/clangd/issues/2148))

  被关联启发式误拾取的散落注释——例如与代码之间隔一空行的节横幅——总是出现在 hover 卡片上：clice 没有配置选项来抑制归属为猜测的文档注释。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace suppression {
  // TODO: tidy this file up.

  int counter;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 宏悬停

<!-- BEGIN GENERATED ITEMS: Macro Hover -->

- [x] 各处的定义文本 — `#define`、使用处、`#ifdef` 和 `#undef` 都显示宏的定义

  宏的 hover 卡片在其名称出现的任何地方都带有 `#define` 文本：定义本身、使用处、`#ifdef` 守卫和 `#undef`。

  <details>
  <summary>示例</summary>

  ```cpp
  int anchor = 0;

  #define LIMIT 64

  int use = LIMIT;

  #ifdef LIMIT
  int guarded = 1;
  #endif

  #undef LIMIT
  ```

  </details>

- [x] 完全展开预览 — 函数式宏使用处显示其参数代入宏体后的结果

  悬停函数式宏调用会显示 `#define` 文本，以及将调用实参代入后完全展开结果的预览。

  <details>
  <summary>示例</summary>

  ```cpp
  int x = 1, y = 2;

  #define MAX(a, b) ((a) > (b) ? (a) : (b))

  int z = MAX(x, y);
  ```

  </details>

- [x] 命令行宏 — `-D` 定义在 hover 中会带有合成的 `#define`

  在命令行定义的宏（`-DFROM_CLI=7`）会在其 hover 卡片中显示合成的 `#define FROM_CLI 7`，然后显示其展开。

  <details>
  <summary>示例</summary>

  ```cpp
  int cli = FROM_CLI;
  ```

  </details>

- [ ] 实参中的嵌套宏 — 在另一宏调用的实参内命名的宏 _(部分)_

  记录的展开从外层调用开始，因此悬停实参内命名的内层宏只会显示其定义，不会显示展开预览。

  <details>
  <summary>示例</summary>

  ```cpp
  int anchor = 0;

  #define ECHO(x) x
  #define INNER_VAL 99

  int nested = ECHO(INNER_VAL);
  ```

  </details>

- [ ] 定义前使用 — 悬停出现在其 `#define` 之前的宏名称 _(部分)_ ([clangd#2642](https://github.com/clangd/clangd/issues/2642))

  在其自身 `#define` 之上的 `#if` 中使用的宏名称仍应在 hover 中显示宏的定义。clice 目前在定义前使用处不返回 hover；`#define` 之后的使用正常。

  <details>
  <summary>示例</summary>

  ```cpp
  int anchor = 0;

  #if COUNT > 0
  int positive = 1;
  #endif

  #define COUNT 3

  int use = COUNT;
  ```

  </details>

- [ ] preamble 内的 `#define` — 悬停位于文件开头的指令

  文件 preamble 区域（第一个声明之前的前导指令串）中的 `#define` 不属于实时解析的预处理器记录，因此悬停其名称不会有任何结果。其他每个宏 fixture 都以声明开头，正是为了将其指令推到 preamble 边界之外。

  <details>
  <summary>示例</summary>

  ```cpp
  #define EARLY 1

  int use = EARLY;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 特殊悬停目标

<!-- BEGIN GENERATED ITEMS: Special Hover Targets -->

- [ ] 类型悬停上的成员 — 悬停枚举或结构体类型时列出其成员 _(部分)_ ([clangd#959](https://github.com/clangd/clangd/issues/959))

  卡片会显示类型名称（以及结构体的布局），但成员列表不会展开——主体渲染为 `{}`。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace members {

  enum Color {
      Red,
      Green,
      Blue,
  };

  struct Point {
      int x;
      int y;
  };

  }
  ```

  </details>

- [ ] typedef 底层结构体 — 悬停别名时展开被别名的定义 _(部分)_ ([clangd#2020](https://github.com/clangd/clangd/issues/2020))

  卡片将别名解析为其底层类型名称，但不会展开该结构体的定义或成员列表。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace aliases {

  struct Widget {
      int id;
      double value;
  };

  using Handle = Widget;

  typedef Widget Widget_t;

  }
  ```

  </details>

- [ ] 关键字文档 — 悬停语言关键字时显示其描述 ([clangd#1862](https://github.com/clangd/clangd/issues/1862))

  悬停 `const` 或 `virtual` 等关键字不会产生卡片。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace keywords {

  const int limit = 42;

  struct Widget {
      virtual void draw();
  };

  }
  ```

  </details>

- [x] 属性文档 — 悬停属性时显示其描述 ([clangd#1862](https://github.com/clangd/clangd/issues/1862))

  属性的文档会渲染在卡片中，适用于 GNU `__attribute__` 写法和 C++ `[[...]]` 属性。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace attr_docs {
  void foo(int * __attribute__((nonnull, noescape)) );

  [[nodiscard]] int compute();
  }
  ```

  </details>

- [x] include 指令悬停 — 悬停 `#include` 时显示解析后的头文件路径

  卡片将引号内的头文件解析为磁盘上的文件。

  <details>
  <summary>示例</summary>

  ```cpp
  #include "own_header.h"

  int use = own_header_value;
  ```

  </details>

- [x] `this` 表达式——悬停 `this` 显示指向的类类型

  适用于普通类和类模板内部。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace this_hover {

  struct Widget {
      Widget* self() {
          return this;
      }
  };

  template <typename T>
  struct Box {
      const Box* self() const {
          return this;
      }
  };

  }
  ```

  </details>

- [x] 预定义标识符——`__func__` hover 显示当前函数名

  在具体函数中值可解析；在模板内只能得到近似类型。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace predefined {

  void current() {
      const char* name = __func__;
  }

  template <int N>
  void generic() {
      const char* name = __func__;
  }

  }
  ```

  </details>

- [x] 无意义 token 不显示悬停——内建关键字和空函数体不产生信息卡

  悬停内建类型关键字或空函数体内部不会产生任何信息卡，因此编辑器显示空白而非噪音。
  （目前数值和 bool 字面量也没有信息卡，但这是已跟踪的差距——见数值字面量条目——不是承诺。）

  <details>
  <summary>示例</summary>

  ```cpp
  namespace negatives {

  int counter = 0;

  void noop() {}

  }
  ```

  </details>

- [ ] GTK-Doc 和 kernel-doc——识别 GObject Introspection 注解（[clangd#2662](https://github.com/clangd/clangd/issues/2662)）

  GTK-Doc / kernel-doc 注释语法和 GObject Introspection 注解不会解析到悬停信息卡中。

  <details>
  <summary>示例</summary>

  ```cpp
  /**
   * gtk_widget_show:
   * @widget: (transfer none): a #GtkWidget
   *
   * Flags a widget to be displayed.
   */
  void gtk_widget_show(GtkWidget *widget);
  ```

  </details>

- [ ] Doxygen 中的 LaTeX 数学公式——渲染 `@f$ ... @f$` 公式（[clangd#2669](https://github.com/clangd/clangd/issues/2669)）

  Doxygen LaTeX 数学公式按原文显示，不渲染为数学公式。

  <details>
  <summary>示例</summary>

  ```cpp
  /// The area of a circle is @f$ A = \pi r^2 @f$.
  double circle_area(double r);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 展示

<!-- BEGIN GENERATED ITEMS: Presentation -->

- [x] Markdown 渲染——信息卡以 markdown 渲染，或通过 `parse_comment_as_markdown = false` 以纯文本渲染

  <details>
  <summary>示例</summary>

  ```cpp
  /// Computes the answer. Tests primality of `p`.
  constexpr int answer(int p) {
      return p + 41;
  }

  int value = answer(1);

  struct Layout {
      char first;
      int second;
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 模块相关

<!-- BEGIN GENERATED ITEMS: Module-Related -->

- [ ] import 语句悬停——悬停 `import` 显示模块信息

  悬停 `import` 声明目前还不能描述被导入的模块。

  <details>
  <summary>示例</summary>

  ```cpp
  export module app;

  import utils;
  ```

  </details>

- [ ] 模块名悬停——悬停模块名列出其所属文件

  悬停模块名目前还不能列出声明它的文件或分区。

  <details>
  <summary>示例</summary>

  ```cpp
  export module math;

  export module math:algebra;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 悬停正确性

在其他工具上出错的输入上保持稳健。

<!-- BEGIN GENERATED ITEMS: Hover Correctness -->

- [x] MSVC 继承模型——`MSInheritanceAttr` 不会破坏类/结构体悬停

  clangd 将此项跟踪为 clangd#1643 和 clangd#2212；在 MSVC 目标下，隐式继承属性不会泄入类/结构体或方法信息卡。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace ms {

  struct Widget {
      int value;
      void update();
  };

  int Widget::* member = &Widget::value;

  }
  ```

  </details>

- [x] Most-vexing-parse——对象初始化和函数声明悬停有区别

  clangd 将此项跟踪为 clangd#2225；clice 将直接初始化读作变量，将引起歧义的形式读作函数声明。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace mvp {

  struct Timer {
      Timer();
      Timer(int);
  };

  int seconds = 5;

  void demo() {
      Timer active(seconds);
      Timer empty();
  }

  }
  ```

  </details>

- [x] 大无符号枚举常量——悬停 `0xFFFF...ULL` 枚举值不会崩溃

  clangd 在此情景下崩溃（clangd#2381）；clice 无溢出地渲染完整无符号值。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace big_enum {

  enum class Flags : unsigned long long {
      Max = 0xFFFFFFFFFFFFFFFFULL,
  };

  }
  ```

  </details>

- [x] 带默认参数的调用——悬停省略默认值的调用不会崩溃

  clangd 在此情景下崩溃（clangd#551）；clice 渲染带默认参数的被调用方签名。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace defaults {

  int compute(int a, int b = 10, int c = 20);

  int result = compute(1);

  }
  ```

  </details>

- [x] 宏遮蔽的符号——与函数同名的函数式宏

  clangd 将此项跟踪为 clangd#2490；在调用点，函数式宏处于活动状态，clice 的信息卡显示该宏及其展开。

  <details>
  <summary>示例</summary>

  ```cpp
  namespace shadow {

  int lookup(int key) {
      return key;
  }

  }

  #define lookup(key) ((key) + 100)

  int value = lookup(5);
  ```

  </details>

<!-- END GENERATED ITEMS -->
