# 代码补全

## 符号

输入时会补全当前作用域内的名称，使用 `ns::` 等限定符则会列出对应作用域中声明的内容。

<!-- BEGIN GENERATED ITEMS: completion/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**模糊非限定查找**

保留匹配度高的前缀匹配项，排除匹配度低的子序列匹配项及作用域外的命名空间成员

```case
tests-next/completion/symbols/01_unqualified_lookup
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板去重**

带有构造函数和推导指引（deduction guide）的类模板仍只显示为一个补全项

```case
tests-next/completion/symbols/02_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏**

对象式宏以常量形式补全，函数式宏以带参数签名的函数形式补全

实参代码片段受函数设置 `code_completion.enable_function_arguments_snippet` 控制。

```case
tests-next/completion/symbols/05_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间限定查找**

`ns::` 列出该命名空间自身的成员

```case
tests-next/completion/symbols/07_namespace_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**枚举成员**

有作用域枚举通过 `Type::` 列出枚举项，无作用域枚举的枚举项则通过其未限定名称补全

```case
tests-next/completion/symbols/08_enum_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**必需的限定符**

无法通过未限定名称查找到的枚举项，会按其未限定名称匹配，并在补全时加上所需的限定符

```case
tests-next/completion/symbols/11_required_qualifier
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [ ] 实参依赖查找（ADL）候选项
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

## 成员访问

由 `.`、`->`、`::` 或 quickSuggestions 触发。通过无状态工作进程转发给 Clang 的 `CodeCompleteConsumer`。

<!-- BEGIN GENERATED ITEMS: completion/member_access -->

<!-- BEGIN CAPABILITY: supported -->

**点号后的成员**

在点号后补全时，会列出对象所属类的成员

```case
tests-next/completion/member_access/01_struct_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作用域限定的成员**

在 `Class::` 后会列出静态数据成员、嵌套类型、方法和注入类名（injected class name）

限定名补全不会只列出静态上下文中可用的成员：实例字段和析构函数也会与静态成员、嵌套类型一同显示。

```case
tests-next/completion/member_access/04_scope_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承的成员**

对派生类对象进行补全时，列出其自身及基类的成员

```case
tests-next/completion/member_access/05_inherited_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**指定初始化器的字段**

输入 `{ .` 会列出正在初始化的聚合体的字段；若其类型依赖于模板参数，则列出匹配的偏特化中的字段

```case
tests-next/completion/member_access/06_designated_initializer
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不可访问的成员**

私有和受保护的成员不会出现在无法使用它们的位置

```case
tests-next/completion/member_access/07_inaccessible_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类外定义**

在定义中输入 `Class::` 后，会列出所有成员，包括私有成员

```case
tests-next/completion/member_access/10_out_of_line_definitions
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

## 函数与代码片段

以下所有选项均位于 `[code_completion]` 配置节中。

<!-- BEGIN GENERATED ITEMS: completion/functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**参数占位符代码片段**

函数调用补全会为每个实参插入可用 Tab 键依次跳转的占位符；无参函数的补全则仍为纯文本

```case
tests-next/completion/functions_snippets/01_snippet_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名与返回类型**

参数列表和返回类型随函数名一起显示

```case
tests-next/completion/functions_snippets/02_function_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重载合并**

一组重载合并为一个条目，并显示重载数量

关闭重载合并后，每个重载都单独列为一个条目，并显示各自的签名。

```case
tests-next/completion/functions_snippets/03_overload_bundle
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**调用括号**

补全时会为调用添加括号，并将光标置于括号内，除非名称后已有实参，或该名称并非用于调用

```case
tests-next/completion/functions_snippets/06_call_parentheses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板实参占位符**

补全类模板时，为每个没有默认值的模板形参插入一个占位符；若所有形参都有默认值，则插入空尖括号

```case
tests-next/completion/functions_snippets/07_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句关键字**

语句关键字补全为关键字；启用该选项后，会插入整条语句，并为每个部分添加占位符

```case
tests-next/completion/functions_snippets/08_statement_snippets
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

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

- [ ] 没有占位符时设置 `InsertTextFormat::PlainText`

## 模板与概念

在模板内部，即使成员依赖于模板形参，也能进行补全。

<!-- BEGIN GENERATED ITEMS: completion/templates -->

<!-- BEGIN CAPABILITY: supported -->

**依赖成员类型**

若变量的类型是依赖于模板形参的特化中的成员类型别名，就能补全该别名所指类的成员

解析别名时，会代入代码中写出的模板实参，因此 `Vec<Vec<T>>::value_type` 会列出 `Vec<T>` 的成员，而不是不显示任何内容。如果别名表示引用（`Vec<Vec<T>>::reference`），则会列出所引用类的成员；对指向这类对象的指针使用 `->`，则会列出所指对象的成员。

```case
tests-next/completion/templates/01_dependent_member_type
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#443 -->

**依赖表达式的结果**

在模板中对下标或成员调用的返回值进行成员访问时，补全该返回值所属类的成员

对于 `Vec<Vec<T>>`，`rows[0].` 会按照标准容器的声明方式，通过容器的 `reference` 别名解析出类型，并列出 `Vec<T>` 的成员。如果成员函数有 `const` 重载，则根据对象是否为 const 来选择调用哪个重载；通过 const 对象访问的数据成员也视为 const，对返回的迭代器使用 `->` 则可访问元素。

```case
tests-next/completion/templates/05_dependent_expression_results
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导变量的成员**

用依赖初始化器声明为 `auto` 的变量，补全其推导出的类的成员

`auto& row = rows[0]; row.` 会列出 `Vec<T>` 的成员。声明符按实际类型推导的规则生效：`const auto&` 使对象具有 const 属性，按值声明的 `auto` 去除初始化器的 const 属性，`auto&&` 和 `decltype(auto)` 保留该属性，而 `auto*` 则取指针所指对象。对于数据成员，`decltype(auto)` 取该成员的声明类型；如果加上括号，则取表达式的 const 引用类型。

```case
tests-next/completion/templates/07_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#493 clangd#1768 -->

**泛型 Lambda 参数**

目前在 Lambda 函数体内，还无法对其 `auto` 参数进行成员补全

调用该 Lambda 时，无论是直接调用还是通过算法调用，传入的实参都是 `Point`，但执行补全时，其函数体仍是模板。

```case
tests-next/completion/templates/14_generic_lambda_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1103 -->

**Concept 要求**

在受约束的模板内，输入点号后可以补全 Concept 所要求的成员

目前只补全成员名称，不包含参数列表。

```case
tests-next/completion/templates/15_concept_requirements
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [ ] 在类模板体内，不为注入类名（injected class name）生成模板参数代码片段
  ```cpp
  template<typename T>
  struct Vec {
      Vec^  // suggest "Vec", not "Vec<${1:T}>" — injected class name
  };
  ```

## 重写与类外定义

在派生类中，基类的虚函数会补全为完整声明。

<!-- BEGIN GENERATED ITEMS: completion/overrides -->

<!-- BEGIN CAPABILITY: supported -->

**重写声明**

在派生类中，基类的虚函数会补全为完整的重写声明，包含返回类型和 `override`

在重写函数内部，函数名会补全为名称本身，而非对基类版本的调用。

```case
tests-next/completion/overrides/01_override_declaration
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

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

- [ ] 在定义上下文中的 `::` 后补全构造函数
- [ ] 不显示类模板构造函数和析构函数的冗余模板参数

  ```cpp
  template<typename T>
  struct Vec { Vec(); ~Vec(); };

  template<typename T>
  Vec<T>::^  // suggest "Vec()" and "~Vec()", not "Vec<T>()" or "~Vec<T>()"
  ```

## 包含路径

由 `<`、`"`、`/` 字符触发。在构建 AST 之前处理（在 Preamble 层面处理，无需编译）。引号内的补全先搜索包含方文件自身所在的目录，再搜索已配置的包含目录。只有看起来像头文件的文件才会成为候选：任何目录下带头文件扩展名的文件，以及系统目录等存放此类头文件的位置中不带扩展名的文件。

<!-- BEGIN GENERATED ITEMS: completion/include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**引号包含路径**

补全会列出已配置的搜索路径中的头文件和目录，并用末尾的斜杠标识目录

```case
tests-next/completion/include_path_completion/01_include_quoted
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尖括号包含路径**

使用尖括号包含头文件时，补全会提供相同搜索路径中的候选项，并以 `>` 闭合

```case
tests-next/completion/include_path_completion/02_include_angled
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**闭合定界符**

接受头文件补全项后，会闭合指令，并替换光标后已输入的内容，避免重复插入

补全会替换已有的定界符；如果在路径中较靠前的部分选择头文件，还会替换路径的剩余部分。

```case
tests-next/completion/include_path_completion/03_closing_delimiter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同目录下的头文件**

使用引号包含头文件时，无需配置搜索路径即可找到与当前文件同目录的头文件

```case
tests-next/completion/include_path_completion/04_sibling_headers
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

<!-- BEGIN GENERATED ITEMS: completion/modules -->

<!-- BEGIN CAPABILITY: supported -->

**导入语句**

在 `import` 后补全已知的模块名，并插入结尾的分号

```case
tests-next/completion/modules/01_import_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**当前模块的分区**

在实现单元中，当前模块自身的分区以 `:partition` 的形式补全，而模块本身和其他模块的分区都不会出现在候选项中

已以分号结尾的语句会保留该分号。

```case
tests-next/completion/modules/02_partition_imports
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**接口单元中的分区**

在接口单元中，只会补全模块的接口分区

接口不能导出内部分区，它从内部分区导入的名称对该接口的导入方也未必可达，因此只有模块的其他单元才会补全内部分区。

```case
tests-next/completion/modules/04_interface_partitions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 以空格字符触发（[#460](https://github.com/clice-io/clice/pull/460)）

  两层门控避免每敲一个空格都触发：服务器把 ` `（空格）注册为触发字符，
  而由空格触发的请求只在导入上下文（`import `、`export import `）中继续处理；
  其余空格立即返回空结果。这与 TypeScript/Haxe 语言扩展采用的模式相同
  （[vscode#67714](https://github.com/microsoft/vscode/issues/67714)）。

- [ ] 按点分层补全

  ```cpp
  import std.^  // suggest io, compat, etc.
  ```

  注意：模块名称中的点是一种命名约定，并不表示语言层面的层级关系，但由点触发的补全仍能改善用户体验。

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

## 过滤与排序

根据输入内容与候选项中各单词开头的匹配情况筛选候选项，匹配度最高的排在最前面。

<!-- BEGIN GENERATED ITEMS: completion/filtering -->

<!-- BEGIN CAPABILITY: supported -->

**下划线过滤**

以下划线开头的内部符号默认隐藏，只有输入的前缀也以下划线开头时才会显示

```case
tests-next/completion/filtering/01_underscore_filter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**弃用标记**

带有 `[[deprecated]]` 的候选项会标记为已弃用，与之对应的不带该属性的候选项则不会

```case
tests-next/completion/filtering/02_deprecated_tag
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词边界模糊匹配**

前缀 `fb` 匹配 `foo_bar_baz` 中各单词的开头

对于 `fb`，`frobnicate` 只能形成较弱的分散子序列匹配，因此被过滤掉；`foo_bar_baz` 在 `foo` 和 `bar` 的单词边界上匹配，因此得以保留。

```case
tests-next/completion/filtering/03_fuzzy_word_boundary
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**前缀匹配优先于子序列匹配**

精确匹配前缀的候选项排在分散子序列匹配的候选项之前

对于前缀 `fo`，`format_output` 属于真正的前缀匹配，得分高于只能按子序列匹配的 `fast_math_operation`。

```case
tests-next/completion/filtering/05_prefix_beats_subsequence
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词中间的补全**

在单词中间补全时，会提供两种编辑范围：编辑器可以在单词剩余部分之前插入补全内容，也可以替换整个单词

```case
tests-next/completion/filtering/06_inside_a_word
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

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

已注册：`. < > : " /` 和空格。

| 字符 | 上下文         | 行为                                |
| ---- | -------------- | ----------------------------------- |
| `.`  | 成员访问       | 语义补全                            |
| `>`  | 通过 `->` 触发 | 指针成员补全；其他位置的 `>` 不触发 |
| `:`  | 通过 `::` 触发 | 作用域补全                          |
| `<`  | `#include <`   | 包含路径补全                        |
| `"`  | `#include "`   | 包含路径补全                        |
| `/`  | 路径分隔符     | 继续补全包含路径                    |
| ` `  | `import` 之后  | 模块名补全（受扩展限制）            |

## 协议

- [ ] `completionItem/resolve`，用于延迟加载文档和详细信息
- [ ] `CompletionList.isIncomplete` 标志，用于增量筛选
- [ ] `commitCharacters`，用于在按下特定按键时自动接受补全项
- [ ] `filterText` / `sortText`，用于在客户端重新筛选
