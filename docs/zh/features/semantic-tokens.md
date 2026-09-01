# 语义 Token

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 用自有的 token 类型词汇表对文档中的每个 token 分类，这套词汇比标准 LSP token 类型更丰富，并且在所有 clice 响应中保持一致。偏好标准 LSP 类型的客户端可以通过配置进行映射。

## 词法 Token

类型来自 token 流本身，不依赖 AST。

<!-- BEGIN GENERATED ITEMS: Lexical Tokens -->

- [x] 注释 — 行注释、块注释和文档注释，包括多行块

  <details>
  <summary>示例</summary>

  ```cpp
  // A line comment.
  /* a one-line block comment */
  /*
   * a block comment
   * spanning several lines
   */
  /// a doc comment
  int after_comments = 0;

  /* first
  second */ int after_block = 1;
  ```

  </details>

- [x] 字面量 — 数字、字符和字符串，包括原始字符串

  <details>
  <summary>示例</summary>

  ```cpp
  int decimal = 42;
  int hexadecimal = 0xFF;
  double floating = 3.14;
  char letter = 'x';
  const char* text = "hello";
  const char* raw = R"(no "escapes" in here)";
  int after_raw = 1;

  const char* multiline = R"(line1
  line2
  )"; int after_closing = 2;
  ```

  </details>

- [x] 关键字 — 包括替代运算符拼写以及上下文相关的 `final` / `override`

  <details>
  <summary>示例</summary>

  ```cpp
  bool logic(bool a, bool b) {
      return a and b or not a;
  }

  struct Base {
      virtual void act();
      virtual ~Base();
  };

  struct Leaf final : Base {
      void act() override;
  };

  struct Last : Base {
      void act() final;
  };
  ```

  </details>

- [x] 预处理指令 — `#if` 链保留指令类型；未启用的分支保留词法类型；pragma 参数保持普通

  <details>
  <summary>示例</summary>

  ```cpp
  int before_conditional = 0;

  #if 0
  int disabled_branch;
  #else
  int enabled_branch = 1;
  #endif

  #define FLAG
  #ifdef FLAG
  int flagged = 2;
  #endif

  #pragma pack(1)

  #
  #define STRINGIZE(x) #x
  const char* stringized = STRINGIZE(abc);
  ```

  </details>

- [x] 非活跃区域 — 未采用分支中的 token 保留词法类型并带有 `inactive` 修饰符；未分类的 token 变为普通的 `identifier` 载体，因此即使是单独一行 `}` 也会变暗

  <details>
  <summary>示例</summary>

  ```cpp
  int before = 0;

  #if 0
  int simple = 1;
  bare identifiers;
  call(arg);
  "string in dead code";
  // comment inside
  #ifdef NESTED
  int deeper = 2;
  #endif
  int tail = 3;
  #endif

  #if defined(MISSING)
  first_branch;
  #elif 0
  elif_branch;
  #else
  int taken = 4;
  #endif

  #if 0
  void edge() {
      inner(5);
  }
  #endif
  ```

  </details>

- [x] 头文件名 — 带引号和尖括号的 `#include` 文件名，包括拆开的 `# include` 形式

  <details>
  <summary>示例</summary>

  ```cpp
  #include "inc/angled.h"
  #include <angled.h>
  # include "inc/angled.h"

  int after_includes = 0;
  ```

  </details>

- [x] 文件顶部的非活跃区域 — 前导指令中未采用的分支以同样方式变暗

  <details>
  <summary>示例</summary>

  ```cpp
  #define KEEP 1
  #if 0
  #define DEAD 2
  #endif

  int after = KEEP;
  ```

  </details>

- [ ] 字面量前缀和后缀 — 编码前缀、类型后缀、数字分隔符和 UDL 后缀作为独立 token

  <details>
  <summary>示例</summary>

  ```cpp
  using size_type = decltype(sizeof(0));
  constexpr size_type operator""_kb(unsigned long long n) {
      return n * 1024;
  }

  auto wide = L"wide string";
  auto utf8 = u8"utf-8 string";
  auto hex = 0xFF;
  auto binary = 0b1010;
  auto unsigned_suffix = 42u;
  auto float_suffix = 3.14f;
  auto separators = 1'000'000;
  auto udl = 4_kb;
  ```

  </details>

- [ ] 转义序列 — 在字符串和字符字面量内部以不同的方式高亮

  <details>
  <summary>示例</summary>

  ```cpp
  const char* escaped = "hello\nworld";
  char hex_escape = '\x41';
  ```

  </details>

- [ ] 声明符与运算符的区分 — `*`、`&`、`&&` 作为声明符还是算术/逻辑运算符 ([clangd#1421](https://github.com/clangd/clangd/issues/1421))

  <details>
  <summary>示例</summary>

  ```cpp
  int value = 1;
  int* pointer = &value;
  int& reference = value;
  int product = value * value;
  int masked = value & 1;
  ```

  </details>

- [x] 原始 token 类型 — 为内置类型使用独立类型，而不是普通的 `keyword`

  <details>
  <summary>示例</summary>

  ```cpp
  int number = 0;
  float ratio = 0.5f;
  void act();
  unsigned long long wide_number = 0;
  __int128 extended_int = 0;
  _Float16 extended_float = 0;
  ```

  </details>

- [ ] 括号 token 类型 — 将匹配的 `()`、`[]`、`{}`、`<>` 对作为不同种类

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Grid {
      T cells[4];
  };

  Grid<int> grid{{1, 2, 3, 4}};

  int first(Grid<int>& grid) {
      return grid.cells[0];
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 声明与引用

名称根据其所定义或引用的声明分类。

<!-- BEGIN GENERATED ITEMS: Declarations & References -->

- [x] 命名空间 — 定义、引用、嵌套命名空间和命名空间别名

  <details>
  <summary>示例</summary>

  ```cpp
  namespace demo {
  namespace inner {
  int value = 1;
  }
  }

  namespace demo::inner::more {}

  namespace alias = demo::inner;

  int use_alias = alias::value;
  ```

  </details>

- [x] 类型 — 类、结构体、联合体、枚举和类型别名，包括定义和引用处

  <details>
  <summary>示例</summary>

  ```cpp
  class Widget {};
  struct Point {};
  union Storage {
      int i;
      float f;
  };
  enum Flags { FlagA };
  enum class Mode { Fast };

  typedef Point PointAlias;
  using WidgetAlias = Widget;

  Widget* make_widget();
  PointAlias origin;
  Mode current = Mode::Fast;
  ```

  </details>

- [x] 函数与方法 — 声明、定义和调用点

  <details>
  <summary>示例</summary>

  ```cpp
  int twice(int value);

  int twice(int value) {
      return value * 2;
  }

  struct Machine {
      void start();
      static void reset();
  };

  void drive(Machine machine) {
      machine.start();
      Machine::reset();
      int four = twice(2);
  }
  ```

  </details>

- [x] 变量 — 全局变量、局部变量、参数、字段和枚举成员

  <details>
  <summary>示例</summary>

  ```cpp
  struct Holder {
      int field;
      static int shared;
  };

  enum class State { Idle };

  int global_value = 1;

  void touch(int param) {
      int local = param + global_value;
      Holder h;
      h.field = local;
      Holder::shared = h.field;
      State state = State::Idle;
  }
  ```

  </details>

- [x] 模板 — 类型与非类型模板参数，模板名称带有 `templated` 修饰符

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T, int N>
  struct Array {
      T data[N];
  };

  template <typename T>
  T identity(T value);

  template <typename T>
  T identity(T value) {
      return value;
  }

  Array<int, 4> arr;
  int result = identity(3);
  ```

  </details>

- [x] 概念 — 定义及作为模板约束的使用

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  concept Small = sizeof(T) <= 4;

  template <Small T>
  void use_small(T value);

  template <typename T>
      requires Small<T>
  void require_small(T value);
  ```

  </details>

- [x] 标签 — `goto` 目标和标签定义

  <details>
  <summary>示例</summary>

  ```cpp
  void retry(bool again) {
      goto done;
  done:
      if (again) {
          goto done;
      }
  }
  ```

  </details>

- [x] 结构化绑定 — 绑定名称在定义和使用处

  前导 `[` 特意不产生 token；仅高亮绑定名称本身。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Pair {
      int first, second;
  };

  void unpack() {
      auto [a, b] = Pair{1, 2};
      int sum = a + b;
  }
  ```

  </details>

- [x] 成员初始化列表 — 初始化字段高亮为字段 ([clangd#122](https://github.com/clangd/clangd/issues/122))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget {
      int width;
      int height;

      Widget(int w, int h) : width(w), height(h) {}
  };
  ```

  </details>

- [x] using 声明 — 引入的名称保持其目标实体的种类 ([clangd#2619](https://github.com/clangd/clangd/issues/2619))

  <details>
  <summary>示例</summary>

  ```cpp
  namespace tools {
  inline int helper() {
      return 1;
  }
  struct Gadget {};
  }

  using tools::helper;
  using tools::Gadget;

  int used = helper();
  Gadget gadget;
  ```

  </details>

- [x] Lambda init-capture — 捕获的名称高亮为变量 ([clangd#868](https://github.com/clangd/clangd/issues/868))

  <details>
  <summary>示例</summary>

  ```cpp
  int compute();

  auto fn = [val = compute()] {
      return val;
  };
  ```

  </details>

- [x] `sizeof...` — 参数包保持其类型参数 token ([clangd#213](https://github.com/clangd/clangd/issues/213))

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename... Ts>
  constexpr auto count = sizeof...(Ts);
  ```

  </details>

- [x] `using enum` — 在 using 位置高亮枚举名 ([clangd#1283](https://github.com/clangd/clangd/issues/1283))

  <details>
  <summary>示例</summary>

  ```cpp
  enum class Color { Red };

  void paint() {
      using enum Color;
      auto c = Red;
  }
  ```

  </details>

- [x] 推导指引 — 高亮指引名和被指引的模板

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Vec {
      template <typename It>
      Vec(It first, It last);
  };

  template <typename It>
  Vec(It, It) -> Vec<int>;
  ```

  </details>

- [x] 显式实例化 — 实例化的模板名及其书写的模板实参高亮，extern 声明和定义同样处理 ([clangd#316](https://github.com/clangd/clangd/issues/316))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget {};

  template <typename T>
  struct Holder {
      T value;
  };

  extern template struct Holder<Widget>;

  template struct Holder<Widget>;
  ```

  </details>

- [ ] 依赖名称 — 在存在已知主模板时通过主模板解析 _(部分实现)_ ([clangd#154](https://github.com/clangd/clangd/issues/154), [clangd#297](https://github.com/clangd/clangd/issues/297))

  已知模板 (`Box<T>`) 的依赖成员解析到主模板的声明并保持其类别。裸模板参数的成员没有候选声明，当前不获得 token；此类名称的启发式着色仍未实现。

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Box {
      using value_type = int;
      static void reset();
      int size() const;
  };

  template <typename T>
  void resolved(Box<T> box) {
      typename Box<T>::value_type item;
      Box<T>::reset();
      box.size();
  }

  template <typename T>
  void unresolved(T value) {
      typename T::value_type item;
      T::reset();
      value.size();
  }
  ```

  </details>

- [x] 变量模板 — 声明、定义、偏特化和全特化

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T, typename U>
  extern int pair_value;

  template <typename T, typename U>
  int pair_value = 2;

  template <typename T>
  extern int pair_value<T, int>;

  template <typename T>
  int pair_value<T, int> = 4;

  template <>
  int pair_value<int, int> = 5;
  ```

  </details>

- [x] 类外成员定义 — 限定名保持方法的类别和修饰符

  <details>
  <summary>示例</summary>

  ```cpp
  struct Gauge {
      int read() const;
      static void reset();
  };

  int Gauge::read() const {
      return 0;
  }

  void Gauge::reset() {}
  ```

  </details>

- [x] 别名模板 — 别名标识符携带类型类别和 `templated` 修饰符

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  using Ptr = T*;

  template <typename T>
  struct Box {};

  template <typename T>
  using BoxPtr = Box<T>*;

  Ptr<int> pointer = nullptr;
  ```

  </details>

- [x] 模板模板参数 — 作为类型声明和使用

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Holder {};

  template <template <typename> class Container, typename T>
  struct Adaptor {
      Container<T> value;
  };

  Adaptor<Holder, int> adaptor;
  ```

  </details>

- [x] Lambda 捕获 — 按拷贝和按引用捕获引用被捕获的变量；`this` 仍为关键字

  <details>
  <summary>示例</summary>

  ```cpp
  struct S {
      int field;

      int compute() {
          int local = 1;
          auto by_copy = [local, this] {
              return local + this->field;
          };
          auto by_reference = [&local] {
              return local;
          };
          return by_copy() + by_reference();
      }
  };
  ```

  </details>

- [x] 基于范围的 for — 循环变量在定义和使用处

  <details>
  <summary>示例</summary>

  ```cpp
  struct List {
      int* begin();
      int* end();
  };

  void iterate(List items) {
      for (auto& item : items) {
          item = 0;
      }
  }
  ```

  </details>

- [x] 枚举底层类型 — 枚举基引用保持其类型类别

  <details>
  <summary>示例</summary>

  ```cpp
  using Byte = unsigned char;

  enum class Flags : Byte { A, B };

  Flags flags = Flags::A;
  ```

  </details>

- [x] 友元声明 — 友元名称解析到其目标；内联友元定义

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget;
  void ping();

  struct Host {
      friend struct Widget;
      friend void ping();
      friend void inline_friend() {}
  };
  ```

  </details>

- [ ] Dependent using declarations — `using T::name` in a template body*（部分）*

  引入的名称及其使用当前不会产生 token；保留的 dependent-name 修饰符尚未输出。

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Derived : T {
      using T::value;

      int use() {
          return value;
      }
  };
  ```

  </details>

- [ ] Function explicit instantiation directives — clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments and the parameter types _（部分）_ ([llvm#191658](https://github.com/llvm/llvm-project/issues/191658))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget {};

  template <typename T>
  void convert(T value) {}

  extern template void convert<Widget>(Widget);

  template void convert<Widget>(Widget);
  ```

  </details>

- [ ] Variable explicit instantiation directives — clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments, even the declarator's type _（部分）_ ([llvm#191658](https://github.com/llvm/llvm-project/issues/191658))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget {};

  template <typename T>
  T zero = T();

  extern template Widget zero<Widget>;

  template Widget zero<Widget>;
  ```

  </details>

- [x] Explicit instantiation member bodies — a dependent name paints as its actual resolution: agreeing kinds keep the modifiers all instantiations share, disagreeing kinds paint a conflict

  <details>
  <summary>示例</summary>

  ```cpp
  struct A {
      static void hit();
  };

  struct B {
      static int hit;
  };

  struct C {
      void hit();
  };

  template <typename T>
  struct D {
      void go() {
          (void)T::hit;
      }
  };

  template struct D<A>;
  template struct D<B>;

  template <typename T>
  struct E {
      void probe(T t) {
          t.hit();
      }
  };

  template struct E<A>;
  template struct E<C>;
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Modules

<!-- BEGIN GENERATED ITEMS: Modules -->

- [x] Module declarations — the contextual `module` keyword, dotted module names and the private fragment

  <details>
  <summary>示例</summary>

  ```cpp
  module;

  export module demo.core;

  export int exported_value = 1;

  module :private;

  int private_value = 2;

  #if 0
  module :private;
  #endif
  ```

  </details>

- [x] Module partitions — partition names in the module declaration

  <details>
  <summary>示例</summary>

  ```cpp
  export module demo.core:part;

  export int partition_value = 1;
  ```

  </details>

- [x] `module` and `import` as identifiers — contextual keywords keep their semantic kinds outside module declarations

  <details>
  <summary>示例</summary>

  ```cpp
  void f() {
      struct module {};
      module m;
      int import = 1;
      int module = 2;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Token Modifiers

<!-- BEGIN GENERATED ITEMS: Token Modifiers -->

- [x] Declaration vs definition — the modifier distinguishes the two

  <details>
  <summary>示例</summary>

  ```cpp
  int measure(int value);

  int measure(int value) {
      return value;
  }

  struct Sensor;

  struct Sensor {};
  ```

  </details>

- [x] Static — class-level members and static locals

  <details>
  <summary>示例</summary>

  ```cpp
  struct Counter {
      static int total;
      static void bump();
      int current;
  };

  void count() {
      static int calls = 0;
      Counter::bump();
      Counter::total = calls;
  }
  ```

  </details>

- [x] Readonly — const and constexpr values, const methods and enum members

  Readonly 目前基于值：指向 const 的指针算作
  readonly，即使指针本身可以改变。

  <details>
  <summary>示例</summary>

  ```cpp
  enum class Level { High };

  const int limit = 10;
  constexpr int bound = 4;

  struct Gauge {
      int read() const;
      void write(int value);
  };

  void probe(const int& in, const int* pointee_const, int* const self_const) {
      Gauge gauge;
      gauge.read();
      gauge.write(limit);
  }
  ```

  </details>

- [x] Virtual 与 abstract — 虚方法、纯虚方法和抽象类

  <details>
  <summary>示例</summary>

  ```cpp
  struct Shape {
      virtual int area();
      virtual int perimeter() = 0;
      virtual ~Shape();
  };

  struct Square : Shape {
      int perimeter() override;
  };

  int measure(Shape& shape) {
      return shape.area() + shape.perimeter();
  }
  ```

  </details>

- [x] Deprecated — `[[deprecated]]` 声明及其使用

  <details>
  <summary>示例</summary>

  ```cpp
  [[deprecated("use next_api")]] void old_api();
  void next_api();

  void migrate() {
      old_api();
  }
  ```

  </details>

- [x] Default library — 系统头文件中声明的符号

  <details>
  <summary>示例</summary>

  ```cpp
  int before_includes = 0;

  #include <syslib.h>

  int used = system_helper();
  ```

  </details>

- [ ] 作用域修饰符 — 函数、类、文件和全局作用域 ([clangd#352](https://github.com/clangd/clangd/issues/352))

  <details>
  <summary>示例</summary>

  ```cpp
  int global_scope;
  static int file_scope;

  struct Foo {
      int class_scope;

      void bar() {
          int function_scope = 0;
      }
  };
  ```

  </details>

- [ ] 可变引用与指针 — 通过非 const 引用或指针传递的参数 ([clangd#839](https://github.com/clangd/clangd/issues/839))

  <details>
  <summary>示例</summary>

  ```cpp
  void modify(int& out);
  void modify_through(int* out);
  void inspect(const int& in);

  void run() {
      int value = 0;
      modify(value);
      modify_through(&value);
      inspect(value);
  }
  ```

  </details>

- [ ] Deduced — 标记推导类型，如 `auto` 和 `decltype`

  <details>
  <summary>示例</summary>

  ```cpp
  auto deduced_int = 1;
  decltype(deduced_int) same_type = 2;
  ```

  </details>

- [ ] 用户定义运算符 — 区分重载运算符与内置运算符 ([clangd#1521](https://github.com/clangd/clangd/issues/1521))

  <details>
  <summary>示例</summary>

  ```cpp
  struct Vec {
      Vec operator+(const Vec& other) const;
  };

  Vec add(Vec a, Vec b) {
      return a + b;
  }

  int add(int a, int b) {
      return a + b;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 冲突与歧义

C++ 允许结构上不同的实体共享同一个名称。当一个书写名称同时指向不同种类的实体时，没有单一 token 类型是正确的；这类名称会获得专用的 **conflict** token 类型，客户端通常以中性颜色显示。

<!-- BEGIN GENERATED ITEMS: Conflict & Ambiguity -->

- [x] 类型 vs 函数 — 同时命名两者的名称显示为 `conflict`

  <details>
  <summary>示例</summary>

  ```cpp
  namespace shop {
  struct Widget {};
  void Widget();
  }

  using shop::Widget;
  ```

  </details>

- [x] 类型 vs 变量 — 同时命名两者的名称显示为 `conflict`

  <details>
  <summary>示例</summary>

  ```cpp
  namespace mixed {
  struct Thing {};
  int Thing;
  }

  using mixed::Thing;
  ```

  </details>

- [x] 同类型重载集合 — 仅命名函数的名称不构成冲突

  <details>
  <summary>示例</summary>

  ```cpp
  namespace ops {
  void apply();
  void apply(int level);
  }

  using ops::apply;

  void run() {
      apply();
      apply(1);
  }
  ```

  </details>

- [x] 注入类名 — 类内部用作构造函数调用的类名

  书写名称显示为类；它隐含的构造函数引用不会额外着色 — `(` 保持无 token 状态。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Widget {
      Widget(int size);

      Widget create() {
          return Widget(42);
      }
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Token 正确性

clice 刻意固定的行为，包括 clangd 曾经出错的问题。

<!-- BEGIN GENERATED ITEMS: Token Correctness -->

- [x] 构造函数与析构函数 — 带 constructor/destructor 修饰符的 method token ([clangd#1509](https://github.com/clangd/clangd/issues/1509), [clangd#2078](https://github.com/clangd/clangd/issues/2078), [clangd#872](https://github.com/clangd/clangd/issues/872))

  析构函数名称显示为两个 token：`~` 携带 method 类型以及声明/定义修饰符，其后的类名保持为对类的引用。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Session {
      Session();
      ~Session();
  };

  Session::Session() {}

  Session::~Session() {}

  void destroy(Session* session) {
      session->~Session();
  }
  ```

  </details>

- [x] 匿名参数 — 未命名参数不产生 token

  未命名参数类型之后的标点保持无 token 状态。

  <details>
  <summary>示例</summary>

  ```cpp
  void take_one(int) {}
  void take_two(int, char* c) {}
  ```

  </details>

- [x] 运算符名称 — `operator` 关键字和调用点的标点保持原样

  运算符的书写名称是关键字加标点，所以不产生名称 token：`operator` 保持其关键字分类，调用点在运算符符号上不产生任何内容。

  <details>
  <summary>示例</summary>

  ```cpp
  struct Value {
      Value& operator=(const Value& other);
      Value operator+(const Value& other) const;
  };

  void combine(Value a, Value b) {
      a = b;
      Value c = a + b;
  }
  ```

  </details>

- [x] 类模板的析构函数 — 模板下的 `~` 形式保持一致

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  struct Holder {
      ~Holder();
  };

  template <typename T>
  Holder<T>::~Holder() {}
  ```

  </details>

- [x] 转换运算符 — 书写为关键字，转换使用不额外着色

  <details>
  <summary>示例</summary>

  ```cpp
  struct Ratio {
      operator double() const;
      explicit operator bool() const;
  };

  double to_double(Ratio ratio) {
      if (ratio) {
          return ratio;
      }
      return double(ratio);
  }
  ```

  </details>

- [x] 模板参数上的伪析构函数 — `~` 不着色；类型名保持其类型

  <details>
  <summary>示例</summary>

  ```cpp
  template <typename T>
  void reset(T* value) {
      value->~T();
  }
  ```

  </details>

- [x] defaulted 和 deleted 成员 — 特殊成员名称保持其定义 token

  <details>
  <summary>示例</summary>

  ```cpp
  struct Session {
      Session() = default;
      Session(const Session&) = delete;
      ~Session() = default;
  };
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 属性

<!-- BEGIN GENERATED ITEMS: Attributes -->

- [ ] 属性名称 — 标准属性和厂商属性，以及它们内部的表达式 ([clangd#2209](https://github.com/clangd/clangd/issues/2209))

  <details>
  <summary>示例</summary>

  ```cpp
  [[nodiscard]] int compute();
  [[deprecated("use v2")]] void old_func();
  [[maybe_unused]] int counter = 0;

  struct [[gnu::packed]] Packed {};
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 宏

宏定义体内部的 token 保持其词法类型；从宏展开处对它们着色属于未来的展开预览特性。

<!-- BEGIN GENERATED ITEMS: Macros -->

- [x] 宏定义与展开

  <details>
  <summary>示例</summary>

  ```cpp
  #define SQUARE(x) ((x) * (x))

  [[maybe_unused]] static int squared = SQUARE(4);
  ```

  </details>

- [x] 展开处与实参 — 展开名称是宏，书写的实参保持其语义，定义体保持词法

  <details>
  <summary>示例</summary>

  ```cpp
  int value = 1;

  #define ID(x) x
  #define CALL helper()

  void helper();

  int copied = ID(value);

  void run() {
      CALL;
  }
  ```

  </details>

- [ ] Object-like 与 function-like 宏 — 区分两种形式的高亮（[clangd#2649](https://github.com/clangd/clangd/issues/2649)）

  <details>
  <summary>示例</summary>

  ```cpp
  #define MAX_SIZE 1024
  #define CHECK(x) ((x) ? 1 : 0)

  int checked = CHECK(MAX_SIZE);
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 其他已知差距

尚未有 fixture 的精选问题：

- `auto` 参数不应被高亮为模板类型参数
  ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- 成员指针中的嵌套名说明符应获得 token
  ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- `::new` 应保持 `new` 关键字高亮
  ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- `co_yield` / `co_await` 在协程返回类型为模板时丢失高亮
  ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- Token 修饰符应应用于重载运算符的操作数
  ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- 依赖模板名（`obj.template get<int>()`）、通过 `using` 从依赖基类导入的成员，以及具有混合种类重载集的依赖名（[clangd#484](https://github.com/clangd/clangd/issues/484)、
  [clangd#686](https://github.com/clangd/clangd/issues/686)、
  [clangd#1057](https://github.com/clangd/clangd/issues/1057)）

## 非活跃代码区域

未采用分支内的每个 token 都带有 `inactive` 修饰符，同时保留其词法种类，因此编辑器可以通过该修饰符的样式使区域变暗，而不丢失底层的语法颜色。死代码中没有分类的 token——裸标识符和普通标点——以未加样式的 `identifier` 类型输出，使整个区域都有 token 覆盖。clice 的 VS Code 扩展开箱即用地渲染变暗区域；其他编辑器直接对修饰符设置样式（例如 Neovim 中的 `@lsp.mod.inactive`）。

- [x] 使非活跃预处理分支变暗（[clangd#132](https://github.com/clangd/clangd/issues/132)）
- [x] 正确处理 `#elif` 链中的非活跃区域边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）
- [x] 在非活跃区域内保留语法高亮（[clangd#1664](https://github.com/clangd/clangd/issues/1664)）
- [x] 保持非活跃区域与注释的区别（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）
- [ ] 不可达代码变暗（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）

## 格式字符串高亮

- [ ] `std::format` / `std::print` 占位符高亮（[clangd#1709](https://github.com/clangd/clangd/issues/1709)）
- [ ] 将无效格式说明符高亮为错误

## 协议支持

- [x] 全文档语义 token（`textDocument/semanticTokens/full`）
- [x] UTF-16 增量编码 token 位置
- [ ] 基于范围的语义 token（`textDocument/semanticTokens/range`）— 仅为可见视口计算 token，对大文件至关重要
- [ ] 增量更新（`textDocument/semanticTokens/full/delta`）— 仅发送自上次响应以来的更改
