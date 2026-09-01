# 代码补全

## Include 路径补全

由 `<`、`"`、`/` 字符触发。在 AST 之前处理（preamble 层，无需编译）。引号补全会搜索已配置的 include 目录，而不是包含者自身所在目录（除非该目录在 include 路径中）。

<!-- BEGIN GENERATED ITEMS: Include Path Completion -->

- [x] 引号 include 路径 — 来自已配置搜索路径的头文件和目录，目录以尾部斜杠标记

  在编译之前由服务器应答，因此该 fixture 只存在服务器路径。

  <details>
  <summary>示例</summary>

  ```cpp
  #include "snap"
  ```

  </details>

- [x] 尖括号 include 路径 — 同一搜索路径候选的尖括号形式

  <details>
  <summary>示例</summary>

  ```cpp
  #include <snap>
  ```

  </details>

<!-- END GENERATED ITEMS -->

**触发上下文**

- [ ] `#include_next` — 必须检测到该指令是 `#include_next`，而不是 `#include`，并调整搜索从提供当前文件的目录 _之后_ 开始

  ```cpp
  // in <bits/stl_vector.h>, provided by /usr/include/c++/14/
  #include_next <^>  // search starts AFTER /usr/include/c++/14/, skipping it
  ```

- [ ] `__has_include()` / `__has_embed()` — 在这些构造内部触发 include 路径补全

  ```cpp
  #if __has_include(<^>)  // suggest headers, same as #include <
  ```

- [ ] `#embed` 指令补全

  ```cpp
  #embed <^>  // suggest embeddable resource files
  ```

**候选项与排序**

- [x] 遍历编译数据库中的编译器搜索路径
- [x] 文件和目录都是候选项；目录通过标签中的尾部 `/` 来区分
- [ ] 过滤已经 include 的头文件

  ```cpp
  #include <vector>
  #include <^>  // should not suggest "vector" again
  ```

- [ ] 降低私有/内部头文件的优先级 — 正常用户不应直接 include 的路径：
  - 单 `_` 前缀：较低优先级（例如 `_ctype.h`）
  - 双 `__` 前缀：更低优先级（编译器内置实现，如 `__config`、`__bit_reference`）
  - 路径中包含 `detail`、`internal`、`impl`、`bits` 等关键词（第三方库私有头文件，如 `boost/detail/`、`bits/stdc++.h`）

  ```cpp
  #include <^>        // __config, _ctype.h, bits/stdc++.h rank near bottom
  #include <boost/^>  // boost/detail/ ranks lower than boost/asio/
  ```

- [ ] 基于路径距离的排序：项目树中离当前文件更近的头文件排名更高

**插入行为**

- [ ] 目录补全不应插入尾部 `/` — 让用户自行输入以重新触发下一级补全（目前 `/` 直接包含在插入文本中，导致编辑器不会自动触发下一轮补全）（[clangd#395](https://github.com/clangd/clangd/issues/395)）

  ```cpp
  #include <sys^>  // accept "sys" → inserts "sys", user types "/" → next completion fires
  ```

## Module 补全

通过文本上下文分析检测。在 AST 之前处理（preamble 层，无需编译）。

### Import

光标位于 `import` 或 `export import` 之后时触发。

<!-- BEGIN GENERATED ITEMS: Module Completion -->

- [x] Import 语句 — 在 `import` 之后补全已知模块名，并插入结尾分号

  由服务端根据其模块映射回答，因此该 fixture 只有服务端路径；先打开同级模块接口，使模块已知。语句保持未终止——行内有 `;` 表示 import 已完成，不提供任何补全。

  <details>
  <summary>示例</summary>

  `main.cpp`：

  ```cpp
  import ma
  ```

  `mod_math.cppm`：

  ```cpp
  export module math;

  export int add(int a, int b) {
      return a + b;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] 空格字符触发（[#460](https://github.com/clice-io/clice/pull/460)）

  双层门控避免每次空格击键都触发：服务端注册 ` `（空格）作为触发字符，空格触发的请求仅在 import 上下文（`import `、`export import `）继续处理；其他空格立即返回空结果。这与 TypeScript/Haxe 语言扩展所用模式相同（[vscode#67714](https://github.com/microsoft/vscode/issues/67714)）。

- [ ] 从结果中排除自身模块（self-import 无效）— **FIXME**
- [ ] 同一模块内的 partition import

  ```cpp
  // inside module foo
  import :^  // suggest :core, :io (only foo's own partitions)
  ```

  注：`import M:part;` 不是合法的 C++ 语法 — partition 只能在同一模块内通过短形式 `import :part;` 导入。

- [ ] 层级 dot 补全

  ```cpp
  import std.^  // suggest io, compat, etc.
  ```

  注：模块名中的点号仅是命名约定，并非语言层面的层级结构，但 dot 触发补全仍是有价值的 UX。

- [ ] 过滤其他模块的非导出（内部）partition
- [ ] Header unit import

  ```cpp
  import <^>  // suggest importable headers (same candidates as #include)
  import "^"  // same, quoted form
  ```

- [ ] 符号补全时自动插入 `import` 语句（类似头文件的 auto-include）

  ```cpp
  std::vector^  // on accept, also insert "import std;" at the top
  ```

### Declaration

模块声明上下文中的补全（`module` / `export module`）。

- [ ] `import` / `module` 关键字补全

  ```cpp
  imp^  // suggest "import" keyword
  mod^  // suggest "module" keyword
  ```

- [ ] `module` / `export module` 后的模块名补全

  ```cpp
  module my^  // suggest existing module names (useful when writing implementation units)
  ```

- [ ] `:` 之后的 partition 名补全

  ```cpp
  export module mylib:^  // suggest existing partition names of mylib
  module mylib:^  // same, for partition implementation unit
  ```

- [ ] `module :private;` 补全（private module fragment）

  ```cpp
  module :^  // suggest "private"
  ```

- [ ] 主接口单元中 `export import :partition` 的 re-export 补全

  ```cpp
  // in primary interface unit of mylib
  export import :^  // suggest mylib's interface partitions that need re-exporting
  ```

## 语义代码补全

由 `.`、`->`、`::` 或 quickSuggestions 触发。通过 stateless worker 转发给 Clang `CodeCompleteConsumer`。

### 成员访问

<!-- BEGIN GENERATED ITEMS: Member Access -->

- [x] 类的成员 — 字段、方法、析构函数和运算符以普通名称补全

  析构函数补全为 `~Account`（绝不会是 `~struct Account`），`operator=` 在 `=` 前不留空格，转换运算符拼出目标类型。

  <details>
  <summary>示例</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  struct Wallet {
      int cents;
  };

  struct Account {
      int balance;
      int bazzzz(int a, int b);
      operator Wallet();
  };

  void bar() {
      Account acc;
      acc.
  }
  ```

  </details>

- [x] 实例化的类模板成员 — 析构函数的 label 保留书写时的模板参数

  <details>
  <summary>示例</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  template <typename T>
  struct Box {
      T value;
  };

  void bar() {
      Box<int> b;
      b.
  }
  ```

  </details>

- [x] 指针成员访问 — 指针上的 `->` 补全所指向对象的成员

  <details>
  <summary>示例</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  struct Node {
      int value;
      Node* next;
      int compute(int a);
  };

  void bar() {
      Node* p;
      p->
  }
  ```

  </details>

- [x] 作用域限定的成员 — 在 `::` 之后，静态数据、嵌套类型、方法和注入类名都会列出

  限定补全不会过滤到静态可达子集：实例字段和析构函数与静态成员及嵌套类型一同出现。

  <details>
  <summary>示例</summary>

  ```cpp
  // The qualified-id is left dangling at the point.
  struct Config {
      static int shared_count;
      static int make(int seed);

      struct Nested {
          int a;
      };

      int instance_field;
  };

  void bar() {
      int v = Config::;
  }
  ```

  </details>

- [x] 继承的成员 — 派生对象补全其自身成员及其基类的成员

  <details>
  <summary>示例</summary>

  ```cpp
  // The member access expression is left dangling at the point.
  struct Base {
      int base_field;
      int base_method();
  };

  struct Derived : Base {
      int derived_field;
  };

  void bar() {
      Derived d;
      d.
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] `->` — 指针成员访问（通过 Clang fixup）
- [x] `::` — 命名空间/类作用域成员
- [ ] Dot-to-arrow：在指针上输入 `.` 触发的 `->` 成员补全并自动替换（[clangd#1349](https://github.com/clangd/clangd/issues/1349)）

  ```cpp
  std::unique_ptr<Foo> ptr;
  ptr.^  // suggest Foo's members, insert as ptr->bar()
  ```

- [ ] 与成员结果一起显示第一个参数匹配对象类型的自由函数

  ```cpp
  std::vector<int> v;
  v.^  // also suggest std::sort(v, ...), std::find(v, ...) etc.
  ```

- [ ] 成员建议中的 `operator[]`、`operator->`、`operator()`
- [ ] 优先显示所输入操作符的直接成员（`.` 时优先 `.` 成员，`->` 时优先 `->` 成员）

### Designated Initializer（指定初始化器）

- [ ] 按声明顺序排序补全结果（C++20 指定初始化器要求如此）（[clangd#965](https://github.com/clangd/clangd/issues/965)）

  ```cpp
  struct Cfg { int width; int height; bool fullscreen; };
  Cfg c = { .^  // suggest: .width, .height, .fullscreen (in this order)
  ```

- [ ] 过滤已使用的 designator

  ```cpp
  Cfg c = { .width = 800, .^  // only suggest .height, .fullscreen
  ```

- [ ] 复合字面量指定初始化器（`(struct T){ .field = }`）
- [ ] 匿名 struct/union 成员 designator

  ```cpp
  struct S { union { int i; float f; }; };
  S s = { .^  // suggest .i, .f
  ```

- [ ] “填充所有成员” snippet

  ```cpp
  Cfg c = { ^  // first item: .width = ${1}, .height = ${2}, .fullscreen = ${3}
  ```

### Override 与类外定义

- [ ] 虚函数 override 补全，带完整签名和 `override` 关键字

  ```cpp
  struct Base { virtual void draw(int x, int y) const; };
  struct Derived : Base {
      ^  // suggest: void draw(int x, int y) const override
  };
  ```

- [ ] 完整继承层次遍历以获取 override 候选（[clangd#226](https://github.com/clangd/clangd/issues/226)、[clangd#2374](https://github.com/clangd/clangd/issues/2374)）

  ```cpp
  struct A { virtual void f(); };
  struct B : A { };
  struct C : B {
      ^  // suggest: void f() override (from A, through B)
  };
  ```

- [ ] 类外定义补全

  ```cpp
  // in .cpp file
  void MyClass::^  // suggest all member functions with full signature + body snippet
  ```

- [ ] 在定义上下文中显示所有成员（包括 private/protected）

  ```cpp
  class Foo { private: void secret(); };
  void Foo::^  // must include "secret" — this is a definition, not a call
  ```

- [ ] 定义上下文中 `::` 后显示构造函数
- [ ] 类模板的构造函数/析构函数省略冗余模板参数

  ```cpp
  template<typename T>
  struct Vec { Vec(); ~Vec(); };

  template<typename T>
  Vec<T>::^  // suggest "Vec()" and "~Vec()", not "Vec<T>()" or "~Vec<T>()"
  ```

### 符号

<!-- BEGIN GENERATED ITEMS: Symbols -->

- [x] 带模糊前缀匹配的非限定查找 — 强前缀匹配保留，弱子序列匹配和未限定的命名空间成员不会保留

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion expression dangles as an unfinished statement.
  namespace A {

  void fooooo();

  }

  struct X {
      void operator()() {}
  };

  void bar() {
      X functor;
      auto folded = [](int x) {
      };
      fo;
  }
  ```

  </details>

- [x] 类模板去重 — 同时是构造函数和推导指引的名称保持为单个类条目

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  template <typename T>
  struct Foo {
      Foo() {}

      Foo(T x) {}

      Foo(T x, T y) {}
  };

  template <typename T>
  Foo(T) -> Foo<T>;

  void bar() {
      Fo
  }
  ```

  </details>

- [x] 构造函数 label 保持简洁 — 类模板构造函数和推导指引补全为裸类名，绝不会是模板化写法

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  template <typename T, typename U>
  struct Bazzz {
      Bazzz() {}

      Bazzz(T x) {}

      Bazzz(T x, U y) {}
  };

  template <typename T>
  Bazzz(T) -> Bazzz<T, int>;

  void bar() {
      Ba
  }
  ```

  </details>

- [x] 关键字模式 — 关键字与其他候选项一样补全，插入纯文本

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int x = tru
  ```

  </details>

- [x] 宏 — object-like 宏作为常量补全，function-like 宏作为带参数签名的函数补全；参数 snippet 遵循函数设置

  <details>
  <summary>示例</summary>

  ```cpp
  #define RETRY_LIMIT 3

  #define CLAMP(value, limit) ((value) < (limit) ? (value) : (limit))

  int a = RETRY;
  int b = CLA;
  ```

  </details>

- [x] 宏遮蔽声明 — 被重定义为宏的名称补全为宏，而不是被遮蔽的声明

  <details>
  <summary>示例</summary>

  ```cpp
  void GUARD(int);
  #define GUARD 1

  int BOUND(int lo, int hi);
  #define BOUND(lo, hi) ((lo) < (hi) ? (lo) : (hi))

  int a = GUAR;
  int b = BOUN;
  ```

  </details>

- [x] 命名空间限定查找 — `ns::` 列出该命名空间自身的成员

  <details>
  <summary>示例</summary>

  ```cpp
  // The qualified-id is left dangling at the point.
  namespace geometry {

  int area_of(int r);

  struct Point {
      int x;
  };

  int origin;

  }  // namespace geometry

  void bar() {
      int v = geometry::;
  }
  ```

  </details>

- [x] 枚举成员 — scoped enum 通过 `Type::` 列出，unscoped enumerator 以裸名称补全

  <details>
  <summary>示例</summary>

  ```cpp
  // Both completion prefixes dangle; the statements stay
  // semicolon-terminated so the second marker is not dragged into recovery.
  enum class Color { Red, Green, Blue };

  enum Fruit { Apple, Banana };

  void bar() {
      Color c = Color::;
      int f = App;
  }
  ```

  </details>

- [x] 局部遮蔽全局 — 被遮蔽的全局不会作为重复条目出现

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int counter = 0;

  void bar() {
      int counter = 1;
      int v = coun;
  }
  ```

  </details>

- [x] 使用声明 — 通过 `using` 引入的名字可以非限定补全

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  namespace lib {

  int helper_fn(int x);

  }

  using lib::helper_fn;

  void bar() {
      int v = help;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] 限定名查找（`std::`）
- [x] 参数相关查找（ADL）候选
- [x] 宏补全 — 候选集中包含对象式宏和函数式宏
- [ ] 带占位符的 snippet 模式（函数体、控制流）
- [ ] C++ attribute 补全

  ```cpp
  [[^]]  // suggest: nodiscard, deprecated, maybe_unused, likely, ...
  ```

- [ ] 跨作用域补全，包括 class/struct 作用域内的符号（内部类型、静态方法）

  ```cpp
  struct Outer { struct Inner {}; static int count; };
  Inn^  // suggest Outer::Inner from a different scope
  ```

- [ ] 在插入的限定符中尊重命名空间别名（优先使用最短有效限定符）

  ```cpp
  namespace fs = std::filesystem;
  fs::ex^  // insert "fs::exists", not "std::filesystem::exists"
  ```

- [ ] 语言感知过滤（混合项目中 C 文件不出现 C++ 符号）
- [ ] 函数参数注释补全（`/*param=*/` 风格的参数提示）
- [ ] 语义分析不可用时基于标识符的回退补全

### 函数与 Snippet

以下所有选项位于 `[code_completion]` 配置节。

<!-- BEGIN GENERATED ITEMS: Functions & Snippets -->

- [x] 签名与返回类型详情 — 参数列表和返回类型作为 label 详情附带显示

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  double foooo(int x, float y);

  int x = fo
  ```

  </details>

- [x] 重载合并 — 重载集合折叠为一个条目并显示重载数量

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);
  double foooo(double d);

  int x = fooo
  ```

  </details>

- [x] 不合并重载 — 关闭合并后，每个重载都是独立条目并带有自己的签名

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);
  double foooo(double d);

  int x = fooo
  ```

  </details>

- [x] 参数占位符 snippet — 调用按参数插入 tab-stop 占位符；无参函数保持纯文本

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefixes dangle as unfinished statements.
  int foooo(int x, float y);
  void nothing_to_fill();

  struct Foo {
      int bazzzz(int a, int b);
  };

  void bar() {
      Foo f;
      fo;
      no;
      f.ba;
  }
  ```

  </details>

- [x] snippet 遵循合并设置 — 重载合并开启时，即使启用了参数 snippet 也不生效

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int foooo(int x);
  int foooo(int x, int y);

  int z = fo
  ```

  </details>

- [x] 带默认值的参数 — 有默认值的参数从签名详情中省略

  签名详情只保留必需参数；尾部的
  `int retries = 3` 被省略。

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int configure(int timeout, int retries = 3);

  int x = confi
  ```

  </details>

- [x] 可变参数签名 — 尾部的 `...` 显示在参数详情中

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  int printf_like(const char* fmt, ...);

  int x = printf
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [ ] 模板参数占位符（`enable_template_arguments_snippet`）
- [ ] 自动插入括号（`insert_paren_in_function_call`）
- [ ] 前瞻检测已有的括号/方括号，避免重复插入

  ```cpp
  foo^(10, 20);  // should NOT insert another pair of parens → foo(10, 20)
  ```

- [ ] 上下文感知 snippet：函数指针上下文中只插入函数名（不加调用语法）

  ```cpp
  void (*fp)(int) = my_fun^;  // insert "my_func", not "my_func(${1:int x})"
  ```

- [ ] 从签名和 snippet 中剥离 C++23 显式对象参数

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show signature "(int x)", not "(this S& self, int x)"
  ```

- [ ] 在签名中显示默认参数值（[clangd#100](https://github.com/clangd/clangd/issues/100)）

  ```cpp
  void open(std::string path, int mode = 0644);
  open(^  // detail shows "(string path, int mode = 0644)"
  ```

- [ ] 将 lambda 类型解析为实际签名

  ```cpp
  auto cmp = [](int a, int b) -> bool { return a < b; };
  cmp^  // show "(int a, int b) -> bool", not "<lambda>"
  ```

- [ ] 解析转发函数的参数（[clangd#447](https://github.com/clangd/clangd/issues/447)）

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(^  // show "(int w, int h)"
  ```

- [ ] `InsertReplaceEdit` 支持（同时提供 insert 和 replace 范围，用于单词中间的补全）

  ```cpp
  refact^orize  // insert: "refactoring^orize", replace: "refactoring"
  ```

- [ ] 无占位符时设置 `InsertTextFormat::PlainText`

### 模板与 Concept

- [ ] Concept 感知补全：从模板参数的 concept 约束中推断可用成员（[clangd#1103](https://github.com/clangd/clangd/issues/1103)）

  ```cpp
  template<typename T>
  concept Drawable = requires(T t) { t.draw(); t.resize(int{}, int{}); };

  template<Drawable T>
  void render(T& widget) {
      widget.^  // suggest draw(), resize() from Drawable concept
  }
  ```

- [ ] 未实例化模板中的依赖类型成员补全

  ```cpp
  template<typename T>
  void process(std::vector<std::vector<T>>& matrix) {
      matrix[0].^  // resolve operator[] → vector<T>&, suggest push_back(), size() etc.
  }
  ```

- [ ] 利用单一实例化信息进行 generic lambda 补全 — 当 generic lambda 仅从一个调用点调用时，使用该调用点的参数类型在 lambda 体内提供补全

  ```cpp
  std::vector<std::string> names;
  std::ranges::sort(names, [](const auto& a, const auto& b) {
      return a.^  // a is deducible as std::string from the single call site
  });
  ```

  ```cpp
  auto results = names | std::views::transform([](const auto& s) {
      return s.^  // s is deducible as std::string
  });
  ```

- [ ] 类模板体内部的注入类名（injected class name）不加模板参数 snippet

  ```cpp
  template<typename T>
  struct Vec {
      Vec^  // suggest "Vec", not "Vec<${1:T}>" — injected class name
  };
  ```

### 宏

- [x] 宏名补全，包括从 preamble 反序列化出的宏
- [x] 宏的模糊匹配（与其他符号使用相同匹配器）
- [x] 正确的 `CompletionItemKind`：function-like 宏为 `Function`，object-like 宏为 `Constant`（[clangd#2002](https://github.com/clangd/clangd/issues/2002)）
- [x] 参数列表作为 function-like 宏的 label detail
- [ ] 将宏定义/展开显示为文档（[clangd#1485](https://github.com/clangd/clangd/issues/1485)）

  ```cpp
  #define MAX_BUF 4096
  MAX^  // completion detail shows: #define MAX_BUF 4096
  ```

- [x] function-like 宏的参数占位符（尊重 snippet 设置）

  ```cpp
  #define CHECK(cond, msg) ...
  CHECK^  // insert: CHECK(${1:cond}, ${2:msg})
  ```

- [ ] 宏参数内部的补全，回退到外围上下文

  ```cpp
  #define WRAP(...) __VA_ARGS__
  WRAP(some_obj.^)  // should still offer some_obj's members
  ```

### 过滤与排序

<!-- BEGIN GENERATED ITEMS: Filtering & Ranking -->

- [x] 下划线过滤 — 以下划线开头的内部符号默认隐藏，除非用户输入的前缀本身以下划线开头

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefixes are undeclared identifiers. The
  // statements stay semicolon-terminated: an unterminated one puts the
  // NEXT marker into a recovery context, which completion drops entirely.
  int _private_thing;
  int public_thing;

  int x = pu;
  int y = _p;
  ```

  </details>

- [x] 弃用标记 — [[deprecated]] 候选带有 Deprecated 标记，普通同名符号没有

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix cuts the initializer mid-expression.
  [[deprecated]] int old_thing(int x);
  int new_thing(int x);

  int z = thing
  ```

  </details>

- [x] 词边界模糊匹配 — 前缀 `fb` 匹配 `foo_bar_baz` 的单词开头

  `frobnicate` 只是 `fb` 的弱散布子序列，被过滤掉；
  `foo_bar_baz` 在 `foo`/`bar` 词边界上匹配，保留下来。

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int foo_bar_baz;
  int frobnicate;

  void bar() {
      int v = fb;
  }
  ```

  </details>

- [x] 大小写不敏感前缀 — 小写前缀匹配混合大小写标识符

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int MyLongName;

  void bar() {
      int v = mylong;
  }
  ```

  </details>

- [x] 前缀优先于子序列 — 精确前缀候选项排在散布子序列匹配之上

  对于前缀 `fo`，`format_output` 是真正的前缀，得分高于
  仅作为子序列匹配的 `fast_math_operation`。

  <details>
  <summary>示例</summary>

  ```cpp
  // The completion prefix dangles as an unfinished statement.
  int format_output;
  int fast_math_operation;

  void bar() {
      int v = fo;
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] 具有词边界感知评分的模糊匹配（camelCase、snake_case）
- [x] 过滤恢复上下文结果（`CCC_Recovery`）
- [ ] 结果数量限制（`CodeCompletionOptions.limit`）
- [ ] Frecency/最近使用提升
- [ ] 将数字-字母边界视为分词点（[clangd#1236](https://github.com/clangd/clangd/issues/1236)）

  ```cpp
  i32^  // should match int32_t (digit-letter boundary: "32" → "t")
  ```

- [ ] 作用域感知的相关性分层：局部变量 > 成员 > 命名空间作用域 > 跨作用域
- [ ] 基于上下文类型提升（期望类型为枚举时建议匹配的枚举成员）（[clangd#462](https://github.com/clangd/clangd/issues/462)）

  ```cpp
  enum Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // boost Red, Green, Blue to top
  ```

- [ ] 在 switch 语句中过滤已使用的枚举值

  ```cpp
  switch (color) {
      case Red: break;
      case ^  // suggest Green, Blue only — Red already used
  ```

- [ ] C++ 模式下 `nullptr` 排在 `NULL` 前面
- [ ] 命名信号提升

  ```cpp
  auto foo = get^;  // boost getFoo() over getBar()
  ```

- [ ] 引用计数与文件距离排序信号
- [ ] 机器学习排序模型

## 自动 Include 插入

尚未实现。补全符号时不会插入 `#include` 指令。

- [ ] 接受补全时为未解析的符号插入 `#include`

  ```cpp
  std::vec^  // on accept "vector", also insert #include <vector> at top of file
  ```

- [ ] 检查传递性 include 图以避免重复 include

  ```cpp
  // <algorithm> already includes <iterator> transitively
  std::back_inserter^  // do NOT insert #include <iterator> again
  ```

- [ ] 上下文感知：前向声明或仅指针/引用用法不插入 include（[clangd#639](https://github.com/clangd/clangd/issues/639)）

  ```cpp
  class Foo;
  Foo*^  // no include needed — forward declaration suffices for pointer
  ```

- [ ] C 文件插入 C 头文件，C++ 文件插入 C++ 头文件

  ```c
  // in a .c file
  size_^  // insert #include <stddef.h>, not #include <cstddef>
  ```

- [ ] 可配置行为：`always` / `iwyu-only` / `never`
- [ ] 优先使用项目相对路径而非绝对路径
- [ ] 尊重 IWYU pragma 和头文件映射
- [ ] 为 C++20 模块符号自动插入 `import`

## 补全项中的文档

尚未实现。补全项不包含文档信息。

- [ ] 从声明和定义中提取文档注释

  ```cpp
  /// @brief Opens a file at the given path.
  /// @param path The file system path.
  void open(std::string path);

  op^  // completion popup shows the @brief doc
  ```

- [ ] 无论定义位于何处（头文件、源文件、索引）都可用
- [ ] 将模板模式的文档传播到实例化
- [ ] 标准库文档集成

## 触发字符

已注册：`. < > : " / *`。空格（` `）已计划但尚未合并（[#460](https://github.com/clice-io/clice/pull/460)）。

| 字符 | 上下文        | 行为                                                                                     |
| ---- | ------------- | ---------------------------------------------------------------------------------------- |
| `.`  | 成员访问      | 语义补全                                                                                 |
| `->` | 指针成员      | `[ ]` 尚未工作 — dot-to-arrow fix-it 未传播                                              |
| `::` | 通过 `:` 触发 | 作用域补全                                                                               |
| `<`  | `#include <`  | Include 路径补全                                                                         |
| `>`  | 模板关闭      | 语义补全                                                                                 |
| `"`  | `#include "`  | Include 路径补全                                                                         |
| `/`  | 路径分隔符    | Include 路径续补                                                                         |
| `*`  | 指针解引用    | 语义补全                                                                                 |
| ` `  | `import` 之后 | Module 名补全（扩展门控）— **待合并 [#460](https://github.com/clice-io/clice/pull/460)** |

## LSP 协议特性

- [ ] `completionItem/resolve` 用于延迟加载文档和详情
- [ ] `CompletionList.isIncomplete` 标志用于增量过滤
- [ ] `commitCharacters` 在特定按键时自动接受补全
- [ ] `filterText` / `sortText` 用于客户端侧重新过滤
