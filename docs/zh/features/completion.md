# 代码补全

## 包含路径

由 `<`、`"`、`/` 字符触发。在构建 AST 之前处理（在 Preamble 层面处理，无需编译）。引号内的补全会搜索已配置的包含目录，不会搜索包含方文件自身所在的目录（除非该目录位于包含路径中）。

<!-- BEGIN GENERATED ITEMS: include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**引号包含路径**

补全会列出已配置搜索路径中的头文件和目录，并在目录末尾添加斜杠作为标记

```snap
tests/snap/code_completion/include_path_completion/01_include_quoted.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尖括号包含路径**

尖括号包含路径的补全会提供相同的搜索路径候选项

```snap
tests/snap/code_completion/include_path_completion/02_include_angled.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

**触发上下文**

- [ ] `#include_next` — 必须识别出指令是 `#include_next` 而非 `#include`，并调整搜索起点，从提供当前文件的目录*之后*的目录开始搜索

  ```cpp
  // in <bits/stl_vector.h>, provided by /usr/include/c++/14/
  #include_next <^>  // search starts AFTER /usr/include/c++/14/, skipping it
  ```

- [ ] `__has_include()` / `__has_embed()` — 在这些结构内部触发包含路径补全

  ```cpp
  #if __has_include(<^>)  // suggest headers, same as #include <
  ```

- [ ] `#embed` 指令补全
  ```cpp
  #embed <^>  // suggest embeddable resource files
  ```

**候选项与排序**

- [x] 遍历编译数据库中的编译器搜索路径
- [x] 文件和目录均可作为候选项；目录通过标签末尾的 `/` 区分
- [ ] 过滤掉已包含的头文件

  ```cpp
  #include <vector>
  #include <^>  // should not suggest "vector" again
  ```

- [ ] 降低私有或内部头文件的优先级，即普通用户不应直接包含的路径：
  - 单个 `_` 前缀：较低优先级（例如 `_ctype.h`）
  - 双 `__` 前缀：更低优先级（编译器内置的内部头文件，如 `__config`、`__bit_reference`）
  - 路径中包含 `detail`、`internal`、`impl`、`bits` 等关键词（第三方库的私有头文件，如 `boost/detail/`、`bits/stdc++.h`）

  ```cpp
  #include <^>        // __config, _ctype.h, bits/stdc++.h rank near bottom
  #include <boost/^>  // boost/detail/ ranks lower than boost/asio/
  ```

- [ ] 按路径距离排序：在项目目录树中，离当前文件越近的头文件排名越靠前

**插入行为**

- [ ] 目录补全不应插入末尾的 `/`，应由用户输入，以再次触发下一级目录的补全（目前 `/` 已包含在插入文本中，导致编辑器无法自动触发下一轮补全）（[clangd#395](https://github.com/clangd/clangd/issues/395)）
  ```cpp
  #include <sys^>  // accept "sys" → inserts "sys", user types "/" → next completion fires
  ```

## 模块导入

通过分析文本上下文检测。在构建 AST 之前处理（在 Preamble 层面处理，无需编译）。

当光标位于 `import` 或 `export import` 之后时触发。

<!-- BEGIN GENERATED ITEMS: module_completion -->

<!-- BEGIN CAPABILITY: supported -->

**导入语句**

在 `import` 之后补全已知模块名，并插入结尾的分号

已包含结尾分号的语句视为完整语句，不再提供模块名候选项。

```snap
tests/snap/code_completion/module_completion/01_import_modules/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 以空格字符触发（[#460](https://github.com/clice-io/clice/pull/460)）

  两层门控避免每敲一个空格都触发：服务器把 ` `（空格）注册为触发字符，
  而由空格触发的请求只在导入上下文（`import `、`export import `）中继续处理；
  其余空格立即返回空结果。这与 TypeScript/Haxe 语言扩展采用的模式相同
  （[vscode#67714](https://github.com/microsoft/vscode/issues/67714)）。

- [ ] 从结果中排除当前模块（模块导入自身是无效的）— **FIXME**
- [ ] 在同一模块内导入分区

  ```cpp
  // inside module foo
  import :^  // suggest :core, :io (only foo's own partitions)
  ```

  注意：`import M:part;` 不是合法的 C++ 语法——分区只能在同一模块内通过简写形式 `import :part;` 导入。

- [ ] 按点分层补全

  ```cpp
  import std.^  // suggest io, compat, etc.
  ```

  注意：模块名称中的点是一种命名约定，并不表示语言层面的层级关系，但由点触发的补全仍能改善用户体验。

- [ ] 过滤掉其他模块中未导出的（内部）分区
- [ ] 导入头文件单元

  ```cpp
  import <^>  // suggest importable headers (same candidates as #include)
  import "^"  // same, quoted form
  ```

- [ ] 补全符号时自动插入 `import` 语句（类似于自动包含头文件）

  ```cpp
  std::vector^  // on accept, also insert "import std;" at the top
  ```

## 模块声明

在模块声明上下文（`module` / `export module`）中进行补全。

- [ ] `import` / `module` 关键字补全

  ```cpp
  imp^  // suggest "import" keyword
  mod^  // suggest "module" keyword
  ```

- [ ] 在 `module` / `export module` 后补全模块名称

  ```cpp
  module my^  // suggest existing module names (useful when writing implementation units)
  ```

- [ ] 在 `:` 后补全分区名称

  ```cpp
  export module mylib:^  // suggest existing partition names of mylib
  module mylib:^  // same, for partition implementation unit
  ```

- [ ] `module :private;` 补全（私有模块片段）

  ```cpp
  module :^  // suggest "private"
  ```

- [ ] 在主接口单元中补全用于重新导出的 `export import :partition`

  ```cpp
  // in primary interface unit of mylib
  export import :^  // suggest mylib's interface partitions that need re-exporting
  ```

## 成员访问

由 `.`、`->`、`::` 或 quickSuggestions 触发。通过无状态工作进程转发给 Clang 的 `CodeCompleteConsumer`。

<!-- BEGIN GENERATED ITEMS: member_access -->

<!-- BEGIN CAPABILITY: supported -->

**类的成员**

字段、方法、析构函数和运算符均以普通名称补全

析构函数补全为 `~Account`（绝不会是 `~struct Account`），`operator=` 中的 `=` 前不加空格，转换运算符则写出其目标类型。

```snap
tests/snap/code_completion/member_access/01_member_access.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**实例化后的类模板成员**

析构函数标签保留代码中写出的模板实参

```snap
tests/snap/code_completion/member_access/02_member_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指针成员访问**

对指针使用 `->` 时，补全其所指对象的成员

```snap
tests/snap/code_completion/member_access/03_pointer_arrow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域限定的成员**

在 `::` 后列出静态数据成员、嵌套类型、方法和注入类名（injected class name）

限定名补全不会只保留无需实例即可访问的成员：实例字段和析构函数也会与静态成员及嵌套类型一起显示。

```snap
tests/snap/code_completion/member_access/04_scope_access.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承的成员**

对派生类对象进行补全时，列出其自身及基类的成员

```snap
tests/snap/code_completion/member_access/05_inherited_members.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] `->`——指针成员访问（带有 Clang 修正）
- [x] `::`——命名空间或类作用域中的成员
- [ ] 点转箭头：在指针后输入 `.` 时，触发 `->` 成员补全并自动替换（[clangd#1349](https://github.com/clangd/clangd/issues/1349)）

  ```cpp
  std::unique_ptr<Foo> ptr;
  ptr.^  // suggest Foo's members, insert as ptr->bar()
  ```

- [ ] 在成员补全结果中同时显示首个参数与对象类型匹配的自由函数（free functions）

  ```cpp
  std::vector<int> v;
  v.^  // also suggest std::sort(v, ...), std::find(v, ...) etc.
  ```

- [ ] 在成员建议中包含 `operator[]`、`operator->`、`operator()`
- [ ] 优先显示通过所输入运算符直接访问的成员（输入 `.` 时优先显示 `.` 可访问的成员，输入 `->` 时优先显示 `->` 可访问的成员）

## 指定初始化器（designated initializers）

- [ ] 按声明顺序排列补全项（C++20 指定初始化器的要求）（[clangd#965](https://github.com/clangd/clangd/issues/965)）

  ```cpp
  struct Cfg { int width; int height; bool fullscreen; };
  Cfg c = { .^  // suggest: .width, .height, .fullscreen (in this order)
  ```

- [ ] 过滤掉已使用的指示符

  ```cpp
  Cfg c = { .width = 800, .^  // only suggest .height, .fullscreen
  ```

- [ ] 复合字面量中的指定初始化器（`(struct T){ .field = }`）
- [ ] 匿名结构体或联合体的成员指示符

  ```cpp
  struct S { union { int i; float f; }; };
  S s = { .^  // suggest .i, .f
  ```

- [ ] “填充所有成员”代码片段

  ```cpp
  Cfg c = { ^  // first item: .width = ${1}, .height = ${2}, .fullscreen = ${3}
  ```

## 重写与类外定义

- [ ] 虚函数重写补全，包含完整签名和 `override` 关键字

  ```cpp
  struct Base { virtual void draw(int x, int y) const; };
  struct Derived : Base {
      ^  // suggest: void draw(int x, int y) const override
  };
  ```

- [ ] 遍历完整的继承层次结构以查找重写候选项（[clangd#226](https://github.com/clangd/clangd/issues/226)、[clangd#2374](https://github.com/clangd/clangd/issues/2374)）

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

- [ ] 在定义上下文中显示所有成员（包括私有成员和受保护成员）

  ```cpp
  class Foo { private: void secret(); };
  void Foo::^  // must include "secret" — this is a definition, not a call
  ```

- [ ] 在定义上下文中的 `::` 后补全构造函数
- [ ] 不显示类模板构造函数和析构函数的冗余模板参数

  ```cpp
  template<typename T>
  struct Vec { Vec(); ~Vec(); };

  template<typename T>
  Vec<T>::^  // suggest "Vec()" and "~Vec()", not "Vec<T>()" or "~Vec<T>()"
  ```

## 符号

<!-- BEGIN GENERATED ITEMS: symbols -->

<!-- BEGIN CAPABILITY: supported -->

**模糊非限定查找**

保留匹配度高的前缀匹配项，排除匹配度低的子序列匹配项和未限定的命名空间成员

```snap
tests/snap/code_completion/symbols/01_unqualified_lookup.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板去重**

同一名称即使也对应构造函数和推导指引，仍只显示为一个类条目

```snap
tests/snap/code_completion/symbols/02_template_dedup.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数标签仅显示类名**

类模板的构造函数和推导指引均补全为不带模板实参的类名，绝不使用带模板实参的形式

```snap
tests/snap/code_completion/symbols/03_constructor_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关键字模式**

关键字与其他候选项一样参与补全，插入内容为纯文本

```snap
tests/snap/code_completion/symbols/04_pattern_keyword.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏**

对象式宏按常量补全，函数式宏按带参数签名的函数补全；实参代码片段遵循函数补全设置

```snap
tests/snap/code_completion/symbols/05_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏遮蔽声明**

名称被重新定义为宏后，补全结果为该宏，而非被遮蔽的声明

```snap
tests/snap/code_completion/symbols/06_macro_shadow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏实参内的补全**

宏实参中的成员访问与宏外的成员访问具有相同的补全行为

```snap
tests/snap/code_completion/symbols/07_macro_argument.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间限定查找**

`ns::` 列出该命名空间自身的成员

```snap
tests/snap/code_completion/symbols/08_namespace_qualified.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**枚举成员**

有作用域枚举通过 `Type::` 列出枚举项，无作用域枚举的枚举项则直接按名称补全

```snap
tests/snap/code_completion/symbols/09_enum_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**局部名称遮蔽全局名称**

被遮蔽的全局名称不会作为重复条目出现

```snap
tests/snap/code_completion/symbols/10_local_shadow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**using 声明**

通过 `using` 引入的名称无需限定即可补全

```snap
tests/snap/code_completion/symbols/11_using_declaration.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 限定名称查找（`std::`）
- [x] 实参依赖查找（ADL）候选项
- [x] 宏补全——候选集包含对象式宏和函数式宏
- [ ] 带占位符的代码片段模式（函数体、控制流）
- [ ] C++ 属性补全

  ```cpp
  [[^]]  // suggest: nodiscard, deprecated, maybe_unused, likely, ...
  ```

- [ ] 跨作用域补全，包含类和结构体作用域内的符号（内部类型、静态方法）

  ```cpp
  struct Outer { struct Inner {}; static int count; };
  Inn^  // suggest Outer::Inner from a different scope
  ```

- [ ] 插入限定符时考虑命名空间别名（优先使用最短的有效限定符）

  ```cpp
  namespace fs = std::filesystem;
  fs::ex^  // insert "fs::exists", not "std::filesystem::exists"
  ```

- [ ] 根据语言过滤候选项（混合语言项目中的 C 文件不显示 C++ 符号）
- [ ] 函数实参注释补全（`/*param=*/` 风格的参数提示）
- [ ] 语义分析不可用时，回退到基于标识符的补全

## 函数与代码片段

以下所有选项均位于 `[code_completion]` 配置节中。

<!-- BEGIN GENERATED ITEMS: functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**签名与返回类型详情**

参数列表和返回类型作为标签详情一同显示

```snap
tests/snap/code_completion/functions_snippets/01_function_candidates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载合并**

一组重载合并为一个条目，并显示重载数量

```snap
tests/snap/code_completion/functions_snippets/02_overload_bundle.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载单独显示**

关闭重载合并后，每个重载各占一个条目，并显示各自的签名

```snap
tests/snap/code_completion/functions_snippets/03_no_bundle_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**参数占位符代码片段**

补全调用时，为每个实参插入可用 Tab 键跳转的占位符；无参函数仍插入为纯文本

```snap
tests/snap/code_completion/functions_snippets/04_snippet_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**代码片段遵循重载合并设置**

合并重载时，即使启用了实参代码片段，也不会使用

```snap
tests/snap/code_completion/functions_snippets/05_snippet_bundle_mode.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**带默认实参的形参**

带默认值的参数不显示在签名详情中

签名详情仅保留必需参数；末尾的 `int retries = 3` 会被省略。

```snap
tests/snap/code_completion/functions_snippets/06_default_argument.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**可变参数签名**

末尾的 `...` 显示在参数详情中

```snap
tests/snap/code_completion/functions_snippets/07_variadic_signature.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [ ] 模板实参占位符（`enable_template_arguments_snippet`）
- [ ] 自动插入圆括号（`insert_paren_in_function_call`）
- [ ] 向后检查是否已有圆括号或方括号，避免重复插入

  ```cpp
  foo^(10, 20);  // should NOT insert another pair of parens → foo(10, 20)
  ```

- [ ] 根据上下文调整代码片段：在函数指针上下文中仅插入名称，不插入调用语法

  ```cpp
  void (*fp)(int) = my_fun^;  // insert "my_func", not "my_func(${1:int x})"
  ```

- [ ] 从签名和代码片段中移除 C++23 显式对象参数

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show signature "(int x)", not "(this S& self, int x)"
  ```

- [ ] 在签名中显示参数默认值（[clangd#100](https://github.com/clangd/clangd/issues/100)）

  ```cpp
  void open(std::string path, int mode = 0644);
  open(^  // detail shows "(string path, int mode = 0644)"
  ```

- [ ] 将 Lambda 类型解析为实际签名

  ```cpp
  auto cmp = [](int a, int b) -> bool { return a < b; };
  cmp^  // show "(int a, int b) -> bool", not "<lambda>"
  ```

- [ ] 解析转发函数的参数（[clangd#447](https://github.com/clangd/clangd/issues/447)）

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(^  // show "(int w, int h)"
  ```

- [ ] 支持 `InsertReplaceEdit`（在单词中间进行代码补全时，同时提供插入范围和替换范围）

  ```cpp
  refact^orize  // insert: "refactoring^orize", replace: "refactoring"
  ```

- [ ] 没有占位符时设置 `InsertTextFormat::PlainText`

## 模板与概念

- [ ] 感知概念约束的代码补全：根据模板参数的概念约束推断可用成员（[clangd#1103](https://github.com/clangd/clangd/issues/1103)）

  ```cpp
  template<typename T>
  concept Drawable = requires(T t) { t.draw(); t.resize(int{}, int{}); };

  template<Drawable T>
  void render(T& widget) {
      widget.^  // suggest draw(), resize() from Drawable concept
  }
  ```

- [ ] 在未实例化的模板中补全待决类型（dependent type）的成员

  ```cpp
  template<typename T>
  void process(std::vector<std::vector<T>>& matrix) {
      matrix[0].^  // resolve operator[] → vector<T>&, suggest push_back(), size() etc.
  }
  ```

- [ ] 利用单次实例化信息为泛型 Lambda 提供代码补全：当泛型 Lambda 仅在一处被调用时，使用该调用位置的实参类型，为 Lambda 函数体内部提供代码补全

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

- [ ] 在类模板体内，不为注入类名（injected class name）生成模板参数代码片段
  ```cpp
  template<typename T>
  struct Vec {
      Vec^  // suggest "Vec", not "Vec<${1:T}>" — injected class name
  };
  ```

## 过滤与排序

<!-- BEGIN GENERATED ITEMS: filtering_ranking -->

<!-- BEGIN CAPABILITY: supported -->

**下划线过滤**

隐藏以下划线开头的内部符号，除非输入的前缀也以下划线开头

```snap
tests/snap/code_completion/filtering_ranking/01_underscore_filter.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**弃用标记**

带有 [[deprecated]] 属性的候选项会标记为弃用，普通的同类候选项则不会

```snap
tests/snap/code_completion/filtering_ranking/02_deprecated_tag.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词边界模糊匹配**

前缀 `fb` 匹配 `foo_bar_baz` 中各单词的开头

`frobnicate` 与 `fb` 仅形成较弱的分散子序列匹配，因此被过滤掉；`foo_bar_baz` 则在 `foo`/`bar` 的单词边界处匹配，因此被保留。

```snap
tests/snap/code_completion/filtering_ranking/03_fuzzy_word_boundary.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不区分大小写的前缀匹配**

小写前缀可以匹配大小写混合的标识符

```snap
tests/snap/code_completion/filtering_ranking/04_case_insensitive.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**前缀匹配优先于子序列匹配**

精确匹配前缀的候选项排在分散子序列匹配的候选项之前

对于前缀 `fo`，`format_output` 属于真正的前缀匹配，得分高于仅形成子序列匹配的 `fast_math_operation`。

```snap
tests/snap/code_completion/filtering_ranking/05_prefix_beats_subsequence.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 模糊匹配评分考虑单词边界（camelCase、snake_case）
- [x] 过滤掉错误恢复上下文的结果（`CCC_Recovery`）
- [ ] 结果数量上限（`CodeCompletionOptions.limit`）
- [ ] 综合使用频率和最近使用时间加权／提高最近使用项的排名
- [ ] 将数字与字母之间的边界视为单词分界（[clangd#1236](https://github.com/clangd/clangd/issues/1236)）

  ```cpp
  i32^  // should match int32_t (digit-letter boundary: "32" → "t")
  ```

- [ ] 按作用域划分相关性层级：局部符号 > 成员 > 命名空间作用域符号 > 跨作用域符号
- [ ] 根据上下文中的类型提升排序优先级（预期类型为枚举时，建议匹配的枚举成员）（[clangd#462](https://github.com/clangd/clangd/issues/462)）

  ```cpp
  enum Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // boost Red, Green, Blue to top
  ```

- [ ] 过滤 switch 语句中已使用的枚举值

  ```cpp
  switch (color) {
      case Red: break;
      case ^  // suggest Green, Blue only — Red already used
  ```

- [ ] 在 C++ 模式下将 `nullptr` 排在 `NULL` 之前
- [ ] 根据命名信号提升排序优先级

  ```cpp
  auto foo = get^;  // boost getFoo() over getBar()
  ```

- [ ] 将引用次数和文件邻近程度作为排序信号
- [ ] 基于机器学习的排序模型

## 自动插入包含指令

尚未实现。补全符号时不会插入 `#include` 指令。

- [ ] 接受补全项时，为未解析的符号插入 `#include`

  ```cpp
  std::vec^  // on accept "vector", also insert #include <vector> at top of file
  ```

- [ ] 检查包含关系图中的传递关系，避免重复包含

  ```cpp
  // <algorithm> already includes <iterator> transitively
  std::back_inserter^  // do NOT insert #include <iterator> again
  ```

- [ ] 感知上下文：对于前置声明或仅通过指针／引用使用的情况，不插入包含指令（[clangd#639](https://github.com/clangd/clangd/issues/639)）

  ```cpp
  class Foo;
  Foo*^  // no include needed — forward declaration suffices for pointer
  ```

- [ ] 在 C 文件中插入 C 头文件，在 C++ 文件中插入 C++ 头文件

  ```c
  // in a .c file
  size_^  // insert #include <stddef.h>, not #include <cstddef>
  ```

- [ ] 可配置的行为：`always` / `iwyu-only` / `never`
- [ ] 优先使用相对于项目的路径，而非绝对路径
- [ ] 遵循 IWYU 编译指示和头文件映射
- [ ] 为 C++20 模块符号自动插入 `import`

## 文档

尚未实现。补全项不包含文档。

- [ ] 从声明和定义中提取文档注释

  ```cpp
  /// @brief Opens a file at the given path.
  /// @param path The file system path.
  void open(std::string path);

  op^  // completion popup shows the @brief doc
  ```

- [ ] 无论定义位于何处（头文件、源文件或索引），都能提供文档
- [ ] 将模板模式（template pattern）的文档传递给实例化结果
- [ ] 集成标准库文档
- [ ] 将宏定义用作文档（[clangd#1485](https://github.com/clangd/clangd/issues/1485)）

## 触发字符

已注册：`. < > : " / *`。空格（` `）的支持处于计划中，但尚未合并（[#460](https://github.com/clice-io/clice/pull/460)）。

| 字符 | 上下文           | 行为                                                                                    |
| ---- | ---------------- | --------------------------------------------------------------------------------------- |
| `.`  | 成员访问         | 语义补全                                                                                |
| `->` | 通过指针访问成员 | `[ ]` 不支持——尚未传递将点号改为箭头的修复建议                                          |
| `::` | 通过 `:` 触发    | 作用域补全                                                                              |
| `<`  | `#include <`     | 包含路径补全                                                                            |
| `>`  | 模板闭合         | 语义补全                                                                                |
| `"`  | `#include "`     | 包含路径补全                                                                            |
| `/`  | 路径分隔符       | 继续补全包含路径                                                                        |
| `*`  | 指针解引用       | 语义补全                                                                                |
| ` `  | `import` 之后    | 模块名补全（受扩展限制）——**计划中 [#460](https://github.com/clice-io/clice/pull/460)** |

## 协议

- [ ] `completionItem/resolve`，用于延迟加载文档和详细信息
- [ ] `CompletionList.isIncomplete` 标志，用于增量筛选
- [ ] `commitCharacters`，用于在按下特定按键时自动接受补全项
- [ ] `filterText` / `sortText`，用于在客户端重新筛选
