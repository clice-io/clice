# 语义 Token

## 词法 Token

通过对源文件的词法扫描生成，不依赖 AST。

- [x] 注释
- [x] 数字、字符、字符串
- [x] 关键字（包括 `override`、`final`）
- [x] 预处理指令（`#if`、`#define` 等）
- [x] `#include` 中的头文件名
- [x] `#define` 处的宏名
- [ ] 字面量前缀与后缀 — 将编码前缀和类型后缀高亮为独立 token

  ```cpp
  R"(raw string)"       // R 前缀
  u8"utf-8 string"      // u8 前缀
  L"wide string"        // L 前缀
  0xFF                   // 0x 前缀
  0b1010                 // 0b 前缀
  42u                    // u 后缀
  3.14f                  // f 后缀
  1'000'000              // 数字分隔符
  ```

- [ ] 用户定义字面量后缀 — 将后缀高亮为独立 token 并关联到字面量运算符

  ```cpp
  using namespace std::chrono_literals;
  auto d = 500ms;        // ms 后缀 → operator""ms
  auto s = "hello"s;     // s 后缀 → operator""s
  auto x = 3.14_deg;     // _deg 后缀 → operator""_deg
  ```

- [ ] 字符串/字符字面量中的转义序列

  ```cpp
  "hello\nworld"         // \n 高亮为不同于周围文本的颜色
  '\x41'                 // \x41 高亮为转义
  ```

- [ ] 声明符 vs 运算符的 `&`、`*`、`&&` 区分 — 区分指针/引用声明符与算术/逻辑运算符（[clangd#1421](https://github.com/clangd/clangd/issues/1421)）

  ```cpp
  int* p;          // * 是声明符（指针类型）
  int x = a * b;   // * 是算术运算符
  int& r = x;      // & 是声明符（引用类型）
  int y = a & b;   // & 是位运算符
  ```

## Token 类型

### 声明与引用

- [x] 命名空间和命名空间别名
- [x] Class、struct、union、enum
- [x] 类型别名（`typedef`、`using`）
- [x] 函数、方法、构造函数、析构函数、运算符
- [x] 变量（局部、全局、参数、字段、绑定）
- [x] 枚举成员
- [x] 模板参数（类型和非类型）
- [x] Concept
- [x] Label（`goto` 目标）
- [ ] Structured binding 变量 — 应高亮为局部变量而非全局（[clangd#485](https://github.com/clangd/clangd/issues/485)）

  ```cpp
  auto [x, y] = point;  // x, y 应高亮为局部变量
  ```

- [ ] 推导指引（deduction guide）

  ```cpp
  template<typename T>
  vector(T, T) -> vector<T>;  // 推导指引应获得 token
  ```

- [ ] `using enum` — 枚举名应获得语义 token（[clangd#1283](https://github.com/clangd/clangd/issues/1283)）

  ```cpp
  using enum Color;  // Color 应高亮为枚举类型
  ```

- [ ] 显式实例化（[clangd#316](https://github.com/clangd/clangd/issues/316)）

  ```cpp
  template class vector<int>;  // vector 和 int 应被高亮
  ```

- [ ] 成员初始化列表中的字段（[clangd#122](https://github.com/clangd/clangd/issues/122)）

  ```cpp
  struct Widget {
      int width, height;
      Widget(int w, int h) : width(w), height(h) {}
      //                      ^^^^^    ^^^^^^ 应高亮为字段
  };
  ```

- [ ] Lambda init-capture — 应一致地获得 token（[clangd#868](https://github.com/clangd/clangd/issues/868)）

  ```cpp
  auto f = [val = compute()] {};  // val 应高亮为局部变量
  ```

- [ ] Using 声明 — 引入的名称应获得一致的 token（[clangd#2619](https://github.com/clangd/clangd/issues/2619)）

  ```cpp
  using std::vector;  // vector 应获得语义 token
  ```

- [ ] `sizeof...` — pack 参数应获得 token（[clangd#213](https://github.com/clangd/clangd/issues/213)）

  ```cpp
  template<typename... Ts>
  constexpr auto count = sizeof...(Ts);  // Ts 应高亮为 templateParameter
  ```

### Module Token

- [x] `import` / `export` / `module` 关键字独立高亮
- [x] import 声明中的模块名
- [ ] 完整的 AST 级 token 解析（当前为词法回退）

### 属性

- [ ] 属性名应获得语义 token

  ```cpp
  [[nodiscard]] int compute();
  [[deprecated("use v2")]] void old_func();
  [[maybe_unused]] int x;
  ```

- [ ] 厂商特定的属性命名空间

  ```cpp
  [[gnu::packed]] struct S {};     // gnu 命名空间
  [[clang::optnone]] void f();    // clang 命名空间
  ```

- [ ] 属性内部的表达式应完整高亮（[clangd#2209](https://github.com/clangd/clangd/issues/2209)）

  ```cpp
  [[assume(ptr != nullptr)]];  // ptr、nullptr 应获得 token
  ```

## Token 修饰符

### 已实现

- [x] 声明 vs 定义区分
- [x] Static（类静态成员、静态局部变量、静态函数）
- [x] Readonly / const
- [x] Deprecated（`[[deprecated]]`）
- [x] Abstract（纯虚方法）
- [x] Virtual
- [x] Default library（来自系统头文件的符号）
- [x] 构造函数 / 析构函数标记
- [x] Templated

### 计划中

- [ ] 作用域修饰符 — 函数作用域、类作用域、文件作用域、全局作用域（[clangd#352](https://github.com/clangd/clangd/issues/352)）

  ```cpp
  int global;                    // globalScope
  static int file_local;         // fileScope
  struct Foo {
      int member;                // classScope
      void bar() {
          int local;             // functionScope
      }
  };
  ```

- [ ] 可变引用 / 可变指针 — 标记通过非 const 引用或指针传递的参数（[clangd#839](https://github.com/clangd/clangd/issues/839)）

  ```cpp
  void modify(int& out);
  modify(x);  // x 应带有 usedAsMutableReference 修饰符
  ```

- [ ] 用户定义运算符修饰符 — 区分内置 vs 用户定义运算符（[clangd#1521](https://github.com/clangd/clangd/issues/1521)）

  ```cpp
  Vec a, b;
  auto c = a + b;   // + 应带有 userDefined 修饰符
  int x = 1 + 2;    // + 是内置运算符，无修饰符
  ```

- [ ] Object-like vs function-like 宏区分（[clangd#2649](https://github.com/clangd/clangd/issues/2649)）

  ```cpp
  #define MAX_SIZE 1024          // object-like → Constant 类型
  #define CHECK(x) assert(x)    // function-like → Function 类型
  ```

- [ ] 上下文相关的 readonly — 值的 const vs 指针级别的 const（[clangd#1585](https://github.com/clangd/clangd/issues/1585)）

  ```cpp
  const int* p;        // p 本身不是 readonly（指针可以改变）
  int* const q;        // q 是 readonly（指针是 const）
  const int* const r;  // r 是 readonly
  ```

## 冲突 / 歧义

C++ 允许结构上不同的实体共享同一个名称。当一个名称引用了不同种类的多个实体时，语义 token 类型无法明确确定。这些情况将标记一个特殊的 **conflict** 修饰符，以中性颜色（如灰色）显示。

- [ ] 通过 `using` 引入的重载名称 — 一个名称可能同时引入类型和函数

  ```cpp
  namespace N {
      struct Widget {};
      void Widget();     // 合法的 C++：函数与结构体同名
  }
  using N::Widget;       // 同时引入两者 → 冲突：类型还是函数？
  ```

- [ ] 跨实例化的依赖名歧义 — 依赖调用在不同实例化中可能解析为不同种类

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo();  // foo 可能是方法，也可能是仿函数数据成员
  }

  struct A { void foo(); };          // A::foo 是方法
  struct B { std::function<void()> foo; };  // B::foo 是变量

  process(a);  // T = A → foo 是方法（函数颜色）
  process(b);  // T = B → foo 是变量（变量颜色）
  // 冲突：没有唯一正确的 token 类型
  ```

- [ ] 注入类名 — 在类内部，类名既可指代类型也可指代构造函数

  ```cpp
  struct Widget {
      Widget(int);
      Widget create() {
          return Widget(42);  // 此处的 Widget：类型？构造函数？
      }
  };
  ```

## 依赖名高亮

未实例化模板体中依赖于模板参数的名称高亮。

- [ ] 依赖类型名（[clangd#154](https://github.com/clangd/clangd/issues/154)、[clangd#214](https://github.com/clangd/clangd/issues/214)）

  ```cpp
  template<typename T>
  void process() {
      typename T::value_type val;  // value_type 应高亮为类型
      T::static_func();            // static_func 应被高亮
  }
  ```

- [ ] 依赖模板名（[clangd#484](https://github.com/clangd/clangd/issues/484)）

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.template get<int>();  // get 应高亮为方法
  }
  ```

- [ ] 基于启发式的着色 — 当依赖名有可能的目标时使用该目标的种类（[clangd#297](https://github.com/clangd/clangd/issues/297)）

  ```cpp
  template<typename T>
  void process(std::vector<T>& v) {
      v.push_back(val);  // push_back 可基于 vector 的已知成员着色为方法
  }
  ```

- [ ] 通过 `using` 从依赖基类导入的成员（[clangd#686](https://github.com/clangd/clangd/issues/686)）

  ```cpp
  template<typename T>
  struct Derived : Base<T> {
      using Base<T>::member;
      void foo() { member(); }  // member 应获得 token
  };
  ```

- [ ] 重载目标的依赖名 — 名称解析到混合种类的重载集时（[clangd#1057](https://github.com/clangd/clangd/issues/1057)）

## 非活跃代码区域

- [ ] 非活跃预处理分支灰显（`#if 0`、false `#ifdef` 等）（[clangd#132](https://github.com/clangd/clangd/issues/132)）

  ```cpp
  #ifdef _WIN32
      // 活跃代码（正常高亮）
  #else
      // 非活跃代码（灰显 / 降低亮度）
  #endif
  ```

- [ ] `#elif` 链中正确的非活跃区域边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）

  ```cpp
  #if defined(A)
      // ...
  #elif defined(B)
      // #elif 指令行本身不应被灰显
  #else
      // ...
  #endif
  ```

- [ ] 非活跃区域内保留语法高亮（[clangd#1664](https://github.com/clangd/clangd/issues/1664)）

  ```cpp
  #if 0
      int x = 42;     // 仍应有关键字/类型着色，只是降低亮度
      std::string s;   // 不应是纯灰色文本
  #endif
  ```

- [ ] 不将非活跃区域视为注释 — 应与实际注释块区分（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）

- [ ] 不可达代码灰显 — 无条件 `return`、`throw`、无限循环之后的代码（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）

  ```cpp
  void foo() {
      return;
      int x = 42;  // 不可达 — 应被灰显
  }
  ```

## 格式字符串高亮

- [ ] `std::format` / `std::print` 格式字符串占位符（[clangd#1709](https://github.com/clangd/clangd/issues/1709)）

  ```cpp
  std::format("Hello, {}! You are {} years old.", name, age);
  //           ^^^^^^  ^^          ^^
  //           字符串  占位符       占位符（不同颜色）
  ```

- [ ] 将无效的格式说明符高亮为错误

## 协议支持

- [x] 全文档语义 token（`textDocument/semanticTokens/full`）
- [x] UTF-16 增量编码 token 位置
- [ ] 基于范围的语义 token（`textDocument/semanticTokens/range`）— 仅计算可见视口内的 token，对大文件性能至关重要
- [ ] 增量更新（`textDocument/semanticTokens/full/delta`）— 仅发送与上次响应相比变化的 token，减少增量编辑时的传输量

## Token 正确性

clangd 中已知的问题，clice 从一开始就应正确处理：

- [x] 构造函数和析构函数使用 `method` token 类型而非 `class`（[clangd#1509](https://github.com/clangd/clangd/issues/1509)）
- [x] 析构函数 token 覆盖前导 `~` 字符（[clangd#2078](https://github.com/clangd/clangd/issues/2078)）
- [x] 析构函数 token 修饰符与声明一致（[clangd#872](https://github.com/clangd/clangd/issues/872)）
- [ ] 对重载运算符的操作数应用 token 修饰符（[clangd#2547](https://github.com/clangd/clangd/issues/2547)）

  ```cpp
  void process(Vec& out);
  out += rhs;  // out 应带有 usedAsMutableReference 修饰符（通过 operator+=）
  ```

- [ ] 不将 lambda/函数中的 `auto` 参数标记为 `typeParameter`（[clangd#1390](https://github.com/clangd/clangd/issues/1390)）

  ```cpp
  auto f = [](auto x) {};  // 此处的 auto 不是模板类型参数
  void g(auto y) {}         // 同理 — 缩写函数模板，不是 typeParameter
  ```

- [ ] 成员指针中的嵌套名说明符应获得 token（[clangd#2235](https://github.com/clangd/clangd/issues/2235)）

  ```cpp
  int Foo::*ptr;  // Foo 应高亮为 class
  ```

- [ ] `::new` — 即使带有全局作用域前缀，`new` 关键字也应获得语义 token（[clangd#1627](https://github.com/clangd/clangd/issues/1627)）

- [ ] `co_yield` / `co_await` 在协程返回类型为模板时不应丢失高亮（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）

  ```cpp
  template<typename T>
  generator<T> gen(T val) {
      co_yield val;  // co_yield 仍应高亮为关键字
  }
  ```

## 变更记录

| 日期 | 变更                                      | PR  |
| ---- | ----------------------------------------- | --- |
| —    | 初始语义 token 类型与修饰符、全文档 token | —   |
