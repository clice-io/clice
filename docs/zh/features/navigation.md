# 代码导航

## Go to Definition

- [x] 基于索引的跨翻译单元 go-to-definition
- [ ] 基于 AST 的本地/未保存符号回退
- [ ] 穿透宏包装导航到底层声明

  ```cpp
  #define DECLARE_HANDLER(name) void name()
  DECLARE_HANDLER(onReady);  // 在 onReady 上 go-to-def → 应到达底层函数
  ```

- [ ] 错误恢复：即使变量类型未解析也能导航到变量定义
- [ ] 未实例化模板中的依赖类型导航

  ```cpp
  template<typename T>
  void process(std::vector<T>& v) {
      v.push_back(val);  // 在 push_back 上 go-to-def → vector::push_back
  }
  ```

- [ ] 模板特化 → 主模板（[clangd#212](https://github.com/clangd/clangd/issues/212)）

  ```cpp
  template<typename T>
  struct Formatter { ... };           // 主模板

  template<>
  struct Formatter<std::string> { };  // 在 Formatter 上 go-to-def → 主模板
  ```

- [ ] `auto` 关键字 → 推导出的类型（[clangd#2055](https://github.com/clangd/clangd/issues/2055)）

  ```cpp
  auto widget = getWidget();
  // 在 auto 上 go-to-def → Widget（推导出的类型）
  ```

### 隐式代码导航

导航到隐式调用的代码定义。C++ 中许多语法构造会生成对构造函数、运算符、转换等的隐式调用。从语法构造（花括号、关键字、运算符 token）导航到实际被调用的函数，对于理解代码的真实执行至关重要。

隐式导航要求有明确的源 token — 如果该 token 已有明确的 go-to-def 目标（例如变量名始终导航到其声明），则无法复用于隐式调用导航。

**关键字**

- [ ] `override` / `final` → 被重写的基类虚函数

  ```cpp
  struct Base { virtual void draw(); };
  struct Derived : Base {
      void draw() override;  // 在 override 上 go-to-def → Base::draw
  };
  ```

- [ ] `break` / `continue` → 所在循环或 switch 的头部（[clangd#1921](https://github.com/clangd/clangd/issues/1921)）

  ```cpp
  for (int i = 0; i < n; i++) {
      for (int j = 0; j < m; j++) {
          if (done) break;     // 在 break 上 go-to-def → 内层 for
          if (skip) continue;  // 在 continue 上 go-to-def → 内层 for
      }
  }
  ```

**构造与析构**

- [ ] 构造函数调用 — 从括号/花括号导航到选中的构造函数

  ```cpp
  struct Widget { Widget(int w, int h); };
  Widget w(800, 600);        // 在 ( 上 go-to-def → Widget(int, int)
  Widget w2{800, 600};       // 在 { 上 go-to-def → 同上
  auto w3 = Widget(1, 2);   // 在 ( 上 go-to-def → 同上
  ```

- [ ] 拷贝/移动构造与赋值

  ```cpp
  Widget a(1, 2);
  Widget b = a;              // 在 = 上 go-to-def → Widget(const Widget&)
  Widget c = std::move(a);   // 在 = 上 go-to-def → Widget(Widget&&)
  b = c;                     // 在 = 上 go-to-def → operator=(const Widget&)
  ```

- [ ] CTAD — 导航到选中的构造函数

  ```cpp
  std::vector v{1, 2, 3};  // 在 { 上 go-to-def → vector(initializer_list<int>)
  ```

- [ ] 聚合初始化 → 结构体定义

  ```cpp
  struct Point { int x, y; };
  auto p = Point{1, 2};  // 在 { 上 go-to-def → Point
  ```

- [ ] `delete` 表达式 → 析构函数

  ```cpp
  delete widget;  // 在 delete 上 go-to-def → Widget::~Widget
  ```

- [ ] `new` 表达式 → 构造函数（如重载则包含自定义 `operator new`）

  ```cpp
  struct Pool {
      static void* operator new(size_t);
  };
  auto* p = new Pool();       // 在 new 上 go-to-def → Pool() 构造函数
                               // 同时：Pool::operator new（如已重载）
  auto* arr = new Pool[10];   // 在 new 上 go-to-def → Pool() 默认构造函数
  ```

- [ ] 成员初始化列表 → 基类和成员的构造函数

  ```cpp
  struct Base { Base(int); };
  struct Logger { Logger(std::string name); };
  struct App : Base {
      Logger logger;
      App() : Base(42), logger("app") {}
      // 在 Base 上 go-to-def → Base::Base(int)
      // 在 logger 上 go-to-def → Logger(std::string)
  };
  ```

- [ ] 委托构造函数

  ```cpp
  struct Widget {
      Widget(int w, int h);
      Widget() : Widget(0, 0) {}  // 在 Widget 上 go-to-def → Widget(int, int)
  };
  ```

- [ ] 继承构造函数 — 导航到通过 `using` 引入的基类构造函数

  ```cpp
  struct Base { Base(int x); Base(int x, int y); };
  struct Derived : Base {
      using Base::Base;  // 在 Base::Base 上 go-to-def → 列出 Base 的构造函数
  };
  ```

- [ ] 返回值隐式构造

  ```cpp
  Widget create() {
      return {800, 600};  // 在 { 上 go-to-def → Widget(int, int)
  }
  ```

- [ ] Lambda init-capture → 构造函数

  ```cpp
  Widget w;
  auto f = [w = std::move(w)] {};     // 在 = 上 go-to-def → Widget(Widget&&)
  auto g = [s = std::string("hi")] {};  // 在 = 上 go-to-def → string(const char*)
  ```

**运算符**

- [ ] 重载运算符 — 从运算符 token 导航到其定义

  ```cpp
  Vec a, b;
  auto c = a + b;   // 在 + 上 go-to-def → Vec::operator+
  a += b;            // 在 += 上 go-to-def → Vec::operator+=
  ++it;              // 在 ++ 上 go-to-def → iterator::operator++
  v[0];              // 在 [ 上 go-to-def → vector::operator[]
  fn(42);            // 在 ( 上 go-to-def → Functor::operator()
  ptr->member;       // 在 -> 上 go-to-def → SmartPtr::operator->
  ```

- [ ] C++20 重写运算符 — 导航到重写使用的实际运算符

  ```cpp
  struct S {
      bool operator==(const S&) const;
      auto operator<=>(const S&) const = default;
  };
  S a, b;
  a != b;   // 在 != 上 go-to-def → S::operator==
  a > b;    // 在 > 上 go-to-def → S::operator<=>
  ```

- [ ] 用户定义字面量

  ```cpp
  using namespace std::chrono_literals;
  auto d = 500ms;  // 在 ms 上 go-to-def → operator""ms
  ```

**转换**

- [ ] 隐式转换运算符 — 从转换被调用的上下文导航

  ```cpp
  struct Guard { explicit operator bool() const; };
  Guard g;
  if (g) {}              // 在 ( 上 go-to-def → Guard::operator bool()
  while (g) {}           // 同上
  !g;                    // 在 ! 上 go-to-def → Guard::operator bool()（[clangd#1931](https://github.com/clangd/clangd/issues/1931)）
  bool ok = bool(g);     // 在 bool( 上 go-to-def → Guard::operator bool()
  ```

- [ ] 类型转换调用构造函数或转换运算符

  ```cpp
  struct Meters { explicit operator double() const; };
  Meters m;
  double d = static_cast<double>(m);  // 在 static_cast 上 go-to-def → Meters::operator double()

  struct Foo { explicit Foo(int); };
  auto f = static_cast<Foo>(42);      // 在 static_cast 上 go-to-def → Foo(int)
  ```

**Range-for 与 structured bindings**

- [ ] Range-based for — 导航到 `begin()`/`end()`

  ```cpp
  std::vector<int> v;
  for (auto& x : v) {}  // 在 : 上 go-to-def → vector::begin
  ```

- [ ] Structured bindings — 导航到底层访问器或字段

  ```cpp
  std::map<int, std::string> m;
  for (auto& [key, val] : m) {}  // 在 key 上 go-to-def → pair::first
                                  // 在 val 上 go-to-def → pair::second
  ```

**协程**

- [ ] `co_await` / `co_yield` / `co_return` → 对应的 awaiter/promise 方法

  ```cpp
  co_await some_awaitable;   // 在 co_await 上 go-to-def → operator co_await() 或 await_resume()
  co_yield value;            // 在 co_yield 上 go-to-def → promise::yield_value()
  co_return result;          // 在 co_return 上 go-to-def → promise::return_value()
  ```

## Go to Declaration

从符号的使用处或定义处导航到其声明。C++ 中许多实体有独立的声明和定义；go-to-declaration 始终导航到声明一侧。

- [ ] 函数 — 从使用处或行外定义导航到声明/原型

  ```cpp
  // widget.h
  class Widget {
      void draw();  // 声明
  };

  // widget.cpp
  void Widget::draw() { }  // 行外定义
  // 从使用处或定义处 go-to-decl → widget.h 中的类内声明
  ```

- [ ] 类和结构体的前向声明

  ```cpp
  class Widget;           // fwd.h 中的前向声明
  class Widget { ... };   // widget.h 中的完整定义
  // 在 Widget 上 go-to-decl（从使用处或定义处）→ 前向声明
  ```

- [ ] 静态数据成员 → 类内声明

  ```cpp
  struct Config {
      static int timeout;    // 声明
  };
  int Config::timeout = 30;  // 类外定义
  // 在 timeout 上 go-to-decl → 类内声明
  ```

- [ ] `extern` 变量 → 声明

  ```cpp
  // globals.h
  extern int log_level;      // 声明
  // globals.cpp
  int log_level = 0;         // 定义
  // 在 log_level 上 go-to-decl → globals.h 中的 extern 声明
  ```

- [ ] 多重声明 — 当实体在多个位置声明时列出所有声明
- [ ] 声明与定义签名不匹配时也能导航（如参数名不同、const 修饰差异）

## Go to Implementation

- [ ] 虚函数 → 所有 override 实现

  ```cpp
  struct Base { virtual void draw(); };
  struct Circle : Base { void draw() override; };
  struct Rect : Base { void draw() override; };
  // 在 Base::draw 上 go-to-impl → 列出 Circle::draw, Rect::draw
  ```

- [ ] 非虚函数声明 → 行外定义（go-to-impl 作为 go-to-def 的超集）（[clangd#854](https://github.com/clangd/clangd/issues/854)）

  ```cpp
  // widget.h
  class Widget {
      void draw();  // 在 draw 上 go-to-impl → widget.cpp 中的行外定义
  };
  ```

- [ ] 模板 duck-type 导航 — 当模板有已知实例化时，跳转到依赖成员调用的具体实现。同样适用于有已知调用点的 generic lambda。

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo();  // 在 foo 上 go-to-impl → 列出 A::foo, B::foo（来自所有实例化）
  }

  process(a);  // T = A
  process(b);  // T = B
  ```

  ```cpp
  std::vector<std::string> names;
  std::ranges::for_each(names, [](const auto& s) {
      s.size();  // 在 size 上 go-to-impl → std::string::size（来自唯一调用点）
  });
  ```

## Go to Type Definition

导航到符号的类型定义。适用于变量、参数、字段等任何具有类型的命名实体。当类型是类型别名或指针类包装时，应自动解包到底层/指向的类型。

- [ ] 局部变量和参数

  ```cpp
  void process(Widget w) {
      auto result = w.compute();
      // 在 w 上 go-to-type-def → Widget
      // 在 result 上 go-to-type-def → compute() 的返回类型
  }
  ```

- [ ] Class/struct 字段

  ```cpp
  struct App { Logger logger; };
  App app;
  // 在 app.logger 上 go-to-type-def → Logger
  ```

- [ ] `auto` 推导类型

  ```cpp
  auto it = map.begin();
  // 在 it 上 go-to-type-def → map::iterator
  ```

- [ ] 智能指针 → 指向类型（[clangd#1026](https://github.com/clangd/clangd/issues/1026)）

  ```cpp
  std::unique_ptr<Widget> w;
  // 在 w 上 go-to-type-def → Widget，而非 unique_ptr
  ```

- [ ] 类型别名 — 解包 `typedef` / `using` 到底层类型定义

  ```cpp
  using Connection = detail::ConnectionImpl;
  Connection conn;
  // 在 conn 上 go-to-type-def → detail::ConnectionImpl（解包别名）
  ```

- [ ] Structured binding 变量

  ```cpp
  std::map<int, Widget> m;
  auto& [id, widget] = *m.begin();
  // 在 id 上 go-to-type-def → int（来自 pair::first）
  // 在 widget 上 go-to-type-def → Widget（来自 pair::second）
  ```

## Find References

- [x] 基于索引的跨翻译单元 find references
- [x] 包含声明选项
- [ ] range-based for 循环的隐式引用（[clangd#1081](https://github.com/clangd/clangd/issues/1081)）

  ```cpp
  struct Container { iterator begin(); iterator end(); };
  for (auto& x : container) {}  // find-refs on begin() 应包含此循环
  ```

- [ ] 隐式构造函数/析构函数调用

  ```cpp
  struct Blob { Blob(); };
  Blob b;  // find-refs on Blob() 应包含此声明
  ```

- [ ] 转发函数穿透引用 — 对构造函数 find-refs 应包含通过 `std::make_unique`、`std::make_shared`、`emplace_back` 等的调用（[clangd#716](https://github.com/clangd/clangd/issues/716)、[clangd#1872](https://github.com/clangd/clangd/issues/1872)）

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(800, 600);  // find-refs on Widget(int, int) 应包含此处
  vec.emplace_back(800, 600);                    // 以及此处
  ```

- [ ] 依赖/模板上下文中的引用（[clangd#258](https://github.com/clangd/clangd/issues/258)、[clangd#675](https://github.com/clangd/clangd/issues/675)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo();  // find-refs on A::foo 应包含此处（来自实例化 T = A）
  }
  ```

- [ ] 读/写分类 — 标注每个引用是读操作还是写操作（[clangd#2139](https://github.com/clangd/clangd/issues/2139)）

  ```cpp
  int x = 0;        // 写
  int y = x + 1;    // 读
  x = y;            // 写
  ```

- [ ] 所在函数上下文 — 每条引用结果附带所在函数名，提升可读性（[clangd#177](https://github.com/clangd/clangd/issues/177)）
- [ ] 宏定义/展开引用（包括在其他宏定义体内的使用）（[clangd#346](https://github.com/clangd/clangd/issues/346)）
- [ ] Label → goto 引用

  ```cpp
  retry:
    if (failed) goto retry;  // 在 retry 标签上 find-refs → 列出所有 goto
  ```

## Call Hierarchy

- [x] Prepare call hierarchy（函数和方法）
- [x] Incoming calls
- [x] Outgoing calls
- [ ] 在 `detail` 字段中显示函数签名
- [ ] 成员函数包含类名

  ```
  // 当前：  "draw" in file.cpp
  // 期望： "Circle::draw" in file.cpp
  ```

- [ ] 跟踪虚派发（`Base::draw` 的调用方应包含通过派生类 override 的调用）
- [ ] 支持非函数目标（变量、枚举常量）（[clangd#1308](https://github.com/clangd/clangd/issues/1308)）
- [ ] lambda 内部的调用

  ```cpp
  auto task = [&] { foo(); };  // foo() 应出现在 foo 的 incoming calls 中
  ```

- [ ] 通过转发函数的构造函数调用 — `make_unique`、`emplace_back` 等应出现在构造函数的 incoming calls 中（[clangd#2242](https://github.com/clangd/clangd/issues/2242)）

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(800, 600);
  // Widget(int, int) 的 incoming calls 应包含此调用点
  ```

## Type Hierarchy

- [x] Prepare type hierarchy（class/struct/enum/union）
- [x] Supertypes（基类）
- [x] Subtypes（派生类）
- [ ] 模板继承（通过模板特化的派生类）

  ```cpp
  template<typename T>
  struct CRTP : Base {};
  // 在 Base 上查看 type hierarchy 应显示 CRTP<T> 为子类型
  ```

- [ ] 在 type hierarchy 条目中显示模板参数（[clangd#31](https://github.com/clangd/clangd/issues/31)）

  ```
  // 当前：  CRTP（Base 的子类型）
  // 期望：  CRTP<Foo>（Base 的子类型）
  ```

## Workspace Symbol

- [ ] 基本的工作区符号搜索
- [ ] 模糊匹配，支持词边界感知评分（camelCase、snake_case）

  ```
  VecIt → 匹配 std::vector<int>::iterator
  strvi → 匹配 std::string_view
  ```

- [ ] 部分限定名搜索（[clangd#550](https://github.com/clangd/clangd/issues/550)）

  ```
  ns::Foo → 匹配 deeply::nested::ns::Foo
  ```

- [ ] 重载函数结果包含参数类型以便区分（[clangd#1344](https://github.com/clangd/clangd/issues/1344)）

  ```
  // 查询："process"
  // 结果：process(int)、process(std::string)、process(Widget&)
  ```

- [ ] 枚举值在枚举作用域下查找（[clangd#931](https://github.com/clangd/clangd/issues/931)）

  ```
  Color::Red → 匹配 Color::Red（即使是非 scoped enum）
  ```

- [ ] 宏的模糊匹配（与其他符号类型一致）（[clangd#914](https://github.com/clangd/clangd/issues/914)）
- [ ] 优先显示底层声明而非类型别名（[clangd#2253](https://github.com/clangd/clangd/issues/2253)）
- [ ] 按 mangled（链接器）名称搜索

## Module 导航

- [ ] `import module_name` → 跳转到模块接口单元（[clangd#2310](https://github.com/clangd/clangd/issues/2310)）

  ```cpp
  import mylib;  // 在 mylib 上 go-to-def → 模块接口单元（export module mylib;）
  ```

- [ ] `import :partition` → 跳转到 partition 单元

  ```cpp
  import :core;  // 在 core 上 go-to-def → partition 单元（export module mylib:core;）
  ```

- [ ] 同一模块的接口单元与实现单元之间互相导航

  ```cpp
  // 接口单元：  export module mylib;
  // 实现单元：  module mylib;
  // 在 mylib 上 go-to-def → 在接口单元和实现单元之间切换
  ```

- [ ] 点分隔模块名 — 各段导航到对应的模块

  ```cpp
  import std.io;
  // 在 io 上 go-to-def → std.io 模块接口
  // 在 std 上 go-to-def → std 模块接口（如存在）
  ```

## 变更记录

| 日期 | 变更                                           | PR  |
| ---- | ---------------------------------------------- | --- |
| —    | 基于索引的 go-to-definition 和 find references | —   |
| —    | Call hierarchy（incoming/outgoing）            | —   |
| —    | Type hierarchy（supertypes/subtypes）          | —   |
