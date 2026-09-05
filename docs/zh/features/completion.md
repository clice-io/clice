# 代码补全

## 包含路径补全

由 `<`、`"`、`/` 字符触发。在 AST 之前处理（Preamble 层级，无需编译）。引号形式的包含路径补全会搜索已配置的包含目录，而不会搜索发出包含指令的文件自身所在目录（除非该目录位于包含路径中）。

<!-- BEGIN GENERATED ITEMS: include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Quoted include paths**

Completion lists headers and directories from the configured search path and
marks directories with a trailing slash

```snap
tests/snap/code_completion/include_path_completion/01_include_quoted.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Angled include paths**

Angled includes offer the same search-path candidates

```snap
tests/snap/code_completion/include_path_completion/02_include_angled.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

**触发上下文**

- [ ] `#include_next`——必须识别出该指令是 `#include_next`，而不是 `#include`，并调整搜索起点，使其从找到当前文件的目录 _之后_ 开始

  ```cpp
  // in <bits/stl_vector.h>, provided by /usr/include/c++/14/
  #include_next <^>  // search starts AFTER /usr/include/c++/14/, skipping it
  ```

- [ ] `__has_include()` / `__has_embed()`——在这些语法结构中触发包含路径补全

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
- [ ] 过滤已包含的头文件

  ```cpp
  #include <vector>
  #include <^>  // should not suggest "vector" again
  ```

- [ ] 降低私有或内部头文件的优先级——正常用户不应直接包含的路径：
  - 单 `_` 前缀：优先级较低（例如 `_ctype.h`）
  - 双 `__` 前缀：优先级更低（编译器内置的内部实现，如 `__config`、`__bit_reference`）
  - 路径中包含 `detail`、`internal`、`impl`、`bits` 等关键词（第三方库的私有头文件，如 `boost/detail/`、`bits/stdc++.h`）

  ```cpp
  #include <^>        // __config, _ctype.h, bits/stdc++.h rank near bottom
  #include <boost/^>  // boost/detail/ ranks lower than boost/asio/
  ```

- [ ] 基于路径距离的排序：项目树中离当前文件更近的头文件排名更高

**插入行为**

- [ ] 目录补全不应插入尾部 `/`——让用户自行输入该字符，从而重新触发下一级补全（目前插入文本中已包含 `/`，这会导致编辑器无法自动触发下一轮补全）（[clangd#395](https://github.com/clangd/clangd/issues/395)）

  ```cpp
  #include <sys^>  // accept "sys" → inserts "sys", user types "/" → next completion fires
  ```

## 模块补全

通过分析文本上下文进行检测。在 AST 之前处理（Preamble 层级，无需编译）。

### 导入

光标位于 `import` 或 `export import` 之后时触发。

<!-- BEGIN GENERATED ITEMS: module_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Import statements**

Known module names complete after `import`, with the closing semicolon
inserted

A statement that already contains its closing semicolon is complete and
offers no module names.

```snap
tests/snap/code_completion/module_completion/01_import_modules/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 由空格字符触发（[#460](https://github.com/clice-io/clice/pull/460)）

  双层门控可避免每次按下空格键都触发补全：服务器将 ` `（空格）注册为触发字符，而空格触发的请求仅在导入上下文（`import `、`export import `）中继续处理；其他空格触发的请求会立即返回空结果。这与 TypeScript/Haxe 语言扩展采用的模式相同（[vscode#67714](https://github.com/microsoft/vscode/issues/67714)）。

- [ ] 从结果中排除自身模块（自导入无效）——**FIXME**
- [ ] 同一模块内的分区导入

  ```cpp
  // inside module foo
  import :^  // suggest :core, :io (only foo's own partitions)
  ```

  注：`import M:part;` 不是合法的 C++——分区只能在同一模块内通过简写形式 `import :part;` 导入。

- [ ] 按层级进行点号补全

  ```cpp
  import std.^  // suggest io, compat, etc.
  ```

  注：模块名中的点号只是一种命名约定，并不表示语言层面的层级结构，但点号触发的补全对用户体验仍很有价值。

- [ ] 过滤掉其他模块中未导出（内部）的分区
- [ ] 头文件单元导入

  ```cpp
  import <^>  // suggest importable headers (same candidates as #include)
  import "^"  // same, quoted form
  ```

- [ ] 符号补全时自动插入 `import` 语句（类似头文件的自动包含）

  ```cpp
  std::vector^  // on accept, also insert "import std;" at the top
  ```

### 声明

模块声明上下文中的代码补全（`module` / `export module`）。

- [ ] `import` / `module` 关键字补全

  ```cpp
  imp^  // suggest "import" keyword
  mod^  // suggest "module" keyword
  ```

- [ ] `module` / `export module` 后的模块名补全

  ```cpp
  module my^  // suggest existing module names (useful when writing implementation units)
  ```

- [ ] `:` 后的分区名补全

  ```cpp
  export module mylib:^  // suggest existing partition names of mylib
  module mylib:^  // same, for partition implementation unit
  ```

- [ ] `module :private;` 补全（私有模块片段）

  ```cpp
  module :^  // suggest "private"
  ```

- [ ] 主接口单元中的 `export import :partition` 再导出补全

  ```cpp
  // in primary interface unit of mylib
  export import :^  // suggest mylib's interface partitions that need re-exporting
  ```

## 语义代码补全

由 `.`、`->`、`::` 或 quickSuggestions 触发，并通过无状态工作线程转发给 Clang `CodeCompleteConsumer`。

### 成员访问

<!-- BEGIN GENERATED ITEMS: member_access -->

<!-- BEGIN CAPABILITY: supported -->

**Members of a class**

Fields, methods, the destructor and operators complete with plain names

The destructor completes as `~Account` (never `~struct Account`),
`operator=` keeps no space before `=`, and a conversion operator
spells its target type.

```snap
tests/snap/code_completion/member_access/01_member_access.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Instantiated class template members**

The destructor label keeps the written template arguments

```snap
tests/snap/code_completion/member_access/02_member_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pointer member access**

`->` on a pointer completes the pointee's members

```snap
tests/snap/code_completion/member_access/03_pointer_arrow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scope-qualified members**

After `::` static data, nested types, methods and the injected class name
all list

Qualified completion is not filtered to the statically-reachable subset:
instance fields and the destructor show up alongside the static members
and nested types.

```snap
tests/snap/code_completion/member_access/04_scope_access.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inherited members**

A derived object completes its own members and those of its base

```snap
tests/snap/code_completion/member_access/05_inherited_members.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] `->` — 指针成员访问（经 Clang 修正）
- [x] `::` — 命名空间/类作用域内的成员
- [ ] 点转箭头（Dot-to-arrow）：在指针表达式后输入 `.` 时，触发 `->` 成员补全并自动替换（[clangd#1349](https://github.com/clangd/clangd/issues/1349)）

  ```cpp
  std::unique_ptr<Foo> ptr;
  ptr.^  // suggest Foo's members, insert as ptr->bar()
  ```

- [ ] 显示首个参数与对象类型匹配的自由函数，并与成员结果一同列出

  ```cpp
  std::vector<int> v;
  v.^  // also suggest std::sort(v, ...), std::find(v, ...) etc.
  ```

- [ ] 成员建议中的 `operator[]`、`operator->`、`operator()`
- [ ] 优先显示与所输入运算符直接对应的成员（输入 `.` 时优先显示 `.` 成员，输入 `->` 时优先显示 `->` 成员）

### 指派初始化器（Designated Initializers）

- [ ] 按声明顺序排序补全结果（C++20 指派初始化器要求如此）（[clangd#965](https://github.com/clangd/clangd/issues/965)）

  ```cpp
  struct Cfg { int width; int height; bool fullscreen; };
  Cfg c = { .^  // suggest: .width, .height, .fullscreen (in this order)
  ```

- [ ] 过滤掉已使用的指派符

  ```cpp
  Cfg c = { .width = 800, .^  // only suggest .height, .fullscreen
  ```

- [ ] 复合字面量的指派初始化器（`(struct T){ .field = }`）
- [ ] 匿名 struct/union 成员的指派符

  ```cpp
  struct S { union { int i; float f; }; };
  S s = { .^  // suggest .i, .f
  ```

- [ ] “填充所有成员”代码片段

  ```cpp
  Cfg c = { ^  // first item: .width = ${1}, .height = ${2}, .fullscreen = ${3}
  ```

### 重写与类外定义

- [ ] 虚函数重写补全，并包含完整签名和 `override` 关键字

  ```cpp
  struct Base { virtual void draw(int x, int y) const; };
  struct Derived : Base {
      ^  // suggest: void draw(int x, int y) const override
  };
  ```

- [ ] 遍历完整的继承层次结构，以获取重写候选项（[clangd#226](https://github.com/clangd/clangd/issues/226)、[clangd#2374](https://github.com/clangd/clangd/issues/2374)）

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

- [ ] 在定义上下文中补全 `::` 后的构造函数
- [ ] 在类模板的构造函数/析构函数中省略冗余模板参数

  ```cpp
  template<typename T>
  struct Vec { Vec(); ~Vec(); };

  template<typename T>
  Vec<T>::^  // suggest "Vec()" and "~Vec()", not "Vec<T>()" or "~Vec<T>()"
  ```

### 符号

<!-- BEGIN GENERATED ITEMS: symbols -->

<!-- BEGIN CAPABILITY: supported -->

**Fuzzy unqualified lookup**

Strong prefix matches survive, weak subsequence matches and unqualified
namespace members do not

```snap
tests/snap/code_completion/symbols/01_unqualified_lookup.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class template deduplication**

A name that is also constructors and a deduction guide stays a single class
entry

```snap
tests/snap/code_completion/symbols/02_template_dedup.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructor labels stay plain**

Class template constructors and deduction guides complete as the bare class
name, never a templated spelling

```snap
tests/snap/code_completion/symbols/03_constructor_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keyword patterns**

Keywords complete like any candidate, with plain insert text

```snap
tests/snap/code_completion/symbols/04_pattern_keyword.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macros**

Object-like macros complete as constants, function-like ones as functions
with a parameter signature; argument snippets follow the function setting

```snap
tests/snap/code_completion/symbols/05_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro shadowing a declaration**

A name redefined as a macro completes as the macro, not the shadowed
declaration

```snap
tests/snap/code_completion/symbols/06_macro_shadow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Completion inside macro arguments**

Member access written as a macro argument completes as it would outside the
macro

```snap
tests/snap/code_completion/symbols/07_macro_argument.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespace-qualified lookup**

`ns::` lists the namespace's own members

```snap
tests/snap/code_completion/symbols/08_namespace_qualified.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum members**

A scoped enum lists through `Type::`, an unscoped enumerator completes by
bare name

```snap
tests/snap/code_completion/symbols/09_enum_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Local shadowing a global**

The shadowed global does not appear as a duplicate entry

```snap
tests/snap/code_completion/symbols/10_local_shadow.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Using-declaration**

A name pulled in with `using` completes unqualified

```snap
tests/snap/code_completion/symbols/11_using_declaration.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 限定名查找（`std::`）
- [x] 实参依赖查找（ADL）候选项
- [x] 宏补全——候选集中包含对象式宏和函数式宏
- [ ] 带占位符的代码片段模式（函数体、控制流）
- [ ] C++ 属性补全

  ```cpp
  [[^]]  // suggest: nodiscard, deprecated, maybe_unused, likely, ...
  ```

- [ ] 跨作用域补全，包括类或结构体作用域内的符号（嵌套类型、静态成员函数）

  ```cpp
  struct Outer { struct Inner {}; static int count; };
  Inn^  // suggest Outer::Inner from a different scope
  ```

- [ ] 插入限定符时使用命名空间别名（优先使用最短的有效限定符）

  ```cpp
  namespace fs = std::filesystem;
  fs::ex^  // insert "fs::exists", not "std::filesystem::exists"
  ```

- [ ] 语言感知过滤（混合项目中的 C 文件不出现 C++ 符号）
- [ ] 函数参数注释补全（`/*param=*/` 风格的参数提示）
- [ ] 语义分析不可用时基于标识符的回退补全

### 函数与代码片段

以下选项均位于 `[code_completion]` 配置节中。

<!-- BEGIN GENERATED ITEMS: functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**Signature and return type details**

The parameter list and return type ride along as label details

```snap
tests/snap/code_completion/functions_snippets/01_function_candidates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overload bundling**

An overload set collapses into one entry with an overload count

```snap
tests/snap/code_completion/functions_snippets/02_overload_bundle.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unbundled overloads**

With bundling off, every overload is its own entry with its own signature

```snap
tests/snap/code_completion/functions_snippets/03_no_bundle_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter placeholder snippets**

Calls insert tab-stop placeholders per argument; a no-argument function
stays plain text

```snap
tests/snap/code_completion/functions_snippets/04_snippet_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Snippets defer to bundling**

While overloads are bundled, argument snippets stay off even when enabled

```snap
tests/snap/code_completion/functions_snippets/05_snippet_bundle_mode.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default-argument parameters**

A parameter with a default value drops out of the signature detail

The signature detail keeps only the required parameters; the trailing
`int retries = 3` is elided.

```snap
tests/snap/code_completion/functions_snippets/06_default_argument.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variadic signature**

A trailing `...` shows in the parameter detail

```snap
tests/snap/code_completion/functions_snippets/07_variadic_signature.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [ ] 模板实参占位符（`enable_template_arguments_snippet`）
- [ ] 自动插入圆括号（`insert_paren_in_function_call`）
- [ ] 向前检查已有的圆括号或方括号，避免重复插入

  ```cpp
  foo^(10, 20);  // should NOT insert another pair of parens → foo(10, 20)
  ```

- [ ] 上下文相关的代码片段：在函数指针上下文中仅插入名称（不含调用语法）

  ```cpp
  void (*fp)(int) = my_fun^;  // insert "my_func", not "my_func(${1:int x})"
  ```

- [ ] 从签名和代码片段中移除 C++23 显式对象形参

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

- [ ] 解析转发函数的形参（[clangd#447](https://github.com/clangd/clangd/issues/447)）

  ```cpp
  struct Widget { Widget(int w, int h); };
  auto p = std::make_unique<Widget>(^  // show "(int w, int h)"
  ```

- [ ] 支持 `InsertReplaceEdit`（同时提供插入范围和替换范围，用于单词中间的补全）

  ```cpp
  refact^orize  // insert: "refactoring^orize", replace: "refactoring"
  ```

- [ ] 无占位符时设置 `InsertTextFormat::PlainText`

### 模板与 Concept

- [ ] Concept 感知代码补全：根据模板参数的 Concept 约束推断可用成员（[clangd#1103](https://github.com/clangd/clangd/issues/1103)）

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

- [ ] 使用单次实例化信息补全泛型 Lambda——当泛型 Lambda 只有一个调用点时，根据该调用点的实参类型在 Lambda 体内提供补全

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

### 过滤与排序

<!-- BEGIN GENERATED ITEMS: filtering_ranking -->

<!-- BEGIN CAPABILITY: supported -->

**Underscore filtering**

Underscore-prefixed internal symbols hide unless the typed prefix itself
starts with one

```snap
tests/snap/code_completion/filtering_ranking/01_underscore_filter.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated tagging**

A [[deprecated]] candidate carries the Deprecated tag, its plain sibling
does not

```snap
tests/snap/code_completion/filtering_ranking/02_deprecated_tag.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Word-boundary fuzzy match**

Prefix `fb` matches the word starts of `foo_bar_baz`

`frobnicate` is only a weak scattered subsequence of `fb` and is dropped;
`foo_bar_baz` matches on the `foo`/`bar` word boundaries and survives.

```snap
tests/snap/code_completion/filtering_ranking/03_fuzzy_word_boundary.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Case-insensitive prefix**

A lowercase prefix matches a mixed-case identifier

```snap
tests/snap/code_completion/filtering_ranking/04_case_insensitive.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Prefix outranks subsequence**

An exact-prefix candidate sorts above a scattered subsequence match

For prefix `fo`, `format_output` is a true prefix and outscores
`fast_math_operation`, which only matches as a subsequence.

```snap
tests/snap/code_completion/filtering_ranking/05_prefix_beats_subsequence.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 采用词边界感知评分的模糊匹配（camelCase、snake_case）
- [x] 过滤掉恢复上下文的结果（`CCC_Recovery`）
- [ ] 结果数量限制（`CodeCompletionOptions.limit`）
- [ ] Frecency/最近使用项加权
- [ ] 将数字与字母之间的边界视为单词分隔点（[clangd#1236](https://github.com/clangd/clangd/issues/1236)）

  ```cpp
  i32^  // should match int32_t (digit-letter boundary: "32" → "t")
  ```

- [ ] 作用域感知的相关性分级：局部符号 > 成员 > 命名空间作用域符号 > 跨作用域符号
- [ ] 基于上下文为类型匹配项加权（预期类型为枚举时，建议与该类型匹配的枚举成员）（[clangd#462](https://github.com/clangd/clangd/issues/462)）

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
- [ ] 命名信号加权

  ```cpp
  auto foo = get^;  // boost getFoo() over getBar()
  ```

- [ ] 引用计数与文件邻近度排序信号
- [ ] 通过机器学习训练的排序模型

## 自动插入包含指令

尚未实现。补全符号时不会插入 `#include` 指令。

- [ ] 确认补全项时为未解析符号插入 `#include`

  ```cpp
  std::vec^  // on accept "vector", also insert #include <vector> at top of file
  ```

- [ ] 检查传递包含图，避免重复包含

  ```cpp
  // <algorithm> already includes <iterator> transitively
  std::back_inserter^  // do NOT insert #include <iterator> again
  ```

- [ ] 根据上下文判断：前向声明或仅通过指针/引用使用时，不插入包含指令（[clangd#639](https://github.com/clangd/clangd/issues/639)）

  ```cpp
  class Foo;
  Foo*^  // no include needed — forward declaration suffices for pointer
  ```

- [ ] 在 C 文件中插入 C 头文件，在 C++ 文件中插入 C++ 头文件

  ```c
  // in a .c file
  size_^  // insert #include <stddef.h>, not #include <cstddef>
  ```

- [ ] 行为可配置为：`always` / `iwyu-only` / `never`
- [ ] 优先使用项目相对路径而非绝对路径
- [ ] 遵循 IWYU pragma 和头文件映射
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
- [ ] 将模板模式的文档传播到模板实例
- [ ] 标准库文档集成
- [ ] 将宏定义作为文档信息显示（[clangd#1485](https://github.com/clangd/clangd/issues/1485)）

## 触发字符

已注册：`. < > : " / *`。空格（` `）也在计划之中，但相关改动尚未合并（[#460](https://github.com/clice-io/clice/pull/460)）。

| 字符 | 上下文        | 行为                                                                                    |
| ---- | ------------- | --------------------------------------------------------------------------------------- |
| `.`  | 成员访问      | 语义补全                                                                                |
| `->` | 指针成员访问  | `[ ]` 尚不可用——未传播将点号改为箭头的 fix-it                                           |
| `::` | 通过 `:` 触发 | 作用域补全                                                                              |
| `<`  | `#include <`  | 包含路径补全                                                                            |
| `>`  | 模板闭合      | 语义补全                                                                                |
| `"`  | `#include "`  | 包含路径补全                                                                            |
| `/`  | 路径分隔符    | 继续补全包含路径                                                                        |
| `*`  | 指针解引用    | 语义补全                                                                                |
| ` `  | `import` 之后 | 模块名补全（需启用扩展）——**待合并 [#460](https://github.com/clice-io/clice/pull/460)** |

## LSP 协议特性

- [ ] `completionItem/resolve` 用于按需加载文档和详细信息
- [ ] `CompletionList.isIncomplete` 标志用于增量过滤
- [ ] `commitCharacters` 用于在按下特定按键时自动接受补全项
- [ ] `filterText` / `sortText` 用于客户端重新过滤
