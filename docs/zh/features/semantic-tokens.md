# 语义 Token

<!-- The cards in the sections below are generated from the documented cases
     in tests/semantic_tokens/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice 为文档中的每个 Token 着色：名称按其声明或指代的实体着色，关键字、字面量、注释、运算符和指令按其写法着色。它使用一套为 C 和 C++ 设计的自有词汇，而不是标准 LSP 的那一套：类型别名、模板类型参数和非类型模板参数是三种不同的种类，`controlFlow` 这样的修饰符则表明关键字、函数或宏如何影响控制流。clice 的 VS Code 和 Neovim 扩展能为这一切着色；其他编辑器只为它们认识的名称着色，可以通过配置把 clice 的名称改成这些名称。

## 种类

| 种类                               | 标记的内容                                                                                                       |
| ---------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| `comment`                          | 注释                                                                                                             |
| `string`、`character`、`number`    | 字面量，包括其前缀和后缀                                                                                         |
| `escape`                           | 字面量中的转义序列：`\n`、`\x41`，以及格式字符串中的 `{{`                                                        |
| `formatSpecifier`                  | 格式字符串中的占位符：`{}`、`{:>8}`、`%d`                                                                        |
| `keyword`                          | 关键字；`module`、`import`、`override` 和 `final` 起关键字作用时也是；以单词拼写的运算符：`sizeof`、`new`、`and` |
| `primitive`                        | 内置类型：`int`、`bool`、`char8_t`                                                                               |
| `operator`                         | 运算符，无论内置还是重载                                                                                         |
| `bracket`                          | 模板实参和模板参数两侧的 `<` 与 `>`                                                                              |
| `directive`、`header`              | 预处理指令；头文件名                                                                                             |
| `namespace`、`module`              | 命名空间或命名空间别名；C++20 模块名                                                                             |
| `class`、`struct`、`union`、`enum` | 类，按声明它的关键字区分                                                                                         |
| `typeAlias`                        | `typedef` 或 `using` 别名                                                                                        |
| `typeParameter`                    | 模板类型参数或模板模板参数                                                                                       |
| `templateParameter`                | 非类型模板参数                                                                                                   |
| `concept`                          | Concept                                                                                                          |
| `function`、`method`               | 自由函数；成员函数，包括构造函数和析构函数                                                                       |
| `variable`、`parameter`、`field`   | 变量、函数参数、数据成员                                                                                         |
| `enumMember`、`label`              | 枚举成员；标签                                                                                                   |
| `macro`、`macroParameter`          | 宏；函数式宏在其定义中的参数                                                                                     |
| `attribute`                        | 属性名：`nodiscard`、`gnu::packed`                                                                               |
| `unknown`                          | 只有实例化才能解析的待决名：`T::reset()`、`value.size()`                                                         |
| `conflict`                         | 同时指代不同种类实体的名称                                                                                       |
| `plain`                            | 未选中分支中没有自身种类的 Token（关闭词法 Token 时则是其中的所有 Token），为携带 `inactive` 而发送              |

## 修饰符

与标准 LSP 修饰符同名的修饰符，无论用在哪个种类上，含义都与标准修饰符相同：

| 修饰符           | 含义                                                                              |
| ---------------- | --------------------------------------------------------------------------------- |
| `declaration`    | 声明实体但不定义它                                                                |
| `definition`     | 定义实体                                                                          |
| `readonly`       | 无法通过该名称修改对象：常量、指向 const 的引用、const 指针、枚举成员、const 方法 |
| `static`         | 静态成员或静态局部变量                                                            |
| `deprecated`     | 实体已弃用                                                                        |
| `abstract`       | 纯虚方法，或含有纯虚方法的类                                                      |
| `defaultLibrary` | 在系统头文件中声明，或由编译器内置                                                |
| `documentation`  | 位于文档注释中                                                                    |
| `modification`   | 实体在此处被修改：被赋值、被自增，或经引用或地址传给非 const 的参数               |

声明所引入的名称都带有其可见范围：`local`（在所在函数或块内）、`member`（在所在类内）、`internal`（在所在翻译单元内，由 `static` 或匿名命名空间造成）或 `global`。

其余修饰符是 clice 自有的，其中一些在不同种类上各有含义：

| 修饰符                      | 作用于                           | 含义                                                                 |
| --------------------------- | -------------------------------- | -------------------------------------------------------------------- |
| `template`                  | 类、别名、变量或函数             | 模板，或模板的特化                                                   |
| `dependent`                 | 任意名称                         | 依赖于模板参数；每个 `unknown` 名称都带有它                          |
| `virtual`                   | 方法；类                         | 虚方法；多态类                                                       |
| `constexpr`                 | 函数、变量或 `templateParameter` | 声明为 `constexpr`、`consteval` 或 `constinit`，或者是非类型模板参数 |
| `constructor`、`destructor` | 方法                             | 构造函数；析构函数                                                   |
| `callable`                  | 宏                               | 函数式宏                                                             |
|                             | 变量、参数或字段                 | 其类型可被调用：函数指针、Lambda、带有 `operator()` 的类             |
| `controlFlow`               | 关键字                           | 转移控制：`if`、`for`、`return`、`throw`、`co_await`、`case`         |
|                             | 函数                             | `[[noreturn]]`                                                       |
|                             | 宏                               | 其展开可能离开函数：`return`、`break`、`goto`、`throw`               |
| `userDefined`               | 运算符；字面量的后缀             | 调用重载运算符；用户定义字面量                                       |
| `declarator`                | 运算符                           | 声明符中的 `*`、`&` 或 `&&`，与表达式中的同名运算符区分开            |
| `operator`                  | 关键字                           | 运算符：`sizeof`、`new`、`static_cast`、`and`、`defined`             |
| `literal`                   | 关键字                           | `true`、`false`、`nullptr`                                           |
| `affix`                     | 字面量                           | 其前缀或后缀：`u8`、`L`、`42u`、`1.5f`                               |
| `empty`                     | 宏                               | 展开为空                                                             |
| `inactive`                  | 任意 Token                       | 位于未选中的预处理分支中，或属于宏丢弃的实参                         |

## 编辑器

- **VS Code**：clice 扩展为每个种类和修饰符声明了主题会着色的 TextMate 作用域，因此 `keyword.controlFlow` 采用主题为控制关键字设定的颜色，`escape` 采用主题为转义序列设定的颜色。
- **Neovim**：clice 插件把 clice 名称对应的 `@lsp.type.*` 和 `@lsp.mod.*` 高亮组链接到配色方案会设置样式的高亮组，作为配色方案可以覆盖的默认值。
- **其他编辑器**（Zed、Kate、Emacs、Sublime Text）为标准 LSP 名称着色。把 clice 的种类重命名为最接近的标准名称，这些种类就能在该编辑器中着色；重命名写在该编辑器传给 clice 的 `initializationOptions` 中，而不是 VS Code 也会读取的 `clice.toml` 中：

```json
{
  "semantic_tokens": {
    "kinds": {
      "character": "string",
      "escape": "string",
      "formatSpecifier": "string",
      "primitive": "type",
      "bracket": "operator",
      "directive": "macro",
      "header": "string",
      "module": "namespace",
      "union": "struct",
      "typeAlias": "type",
      "templateParameter": "parameter",
      "concept": "type",
      "field": "property",
      "macroParameter": "parameter",
      "attribute": "decorator"
    }
  }
}
```

只有为 C 和 C++ 把 `semantic_tokens` 设为 `"combined"`（或 `"full"`）后，Zed 才会请求语义 Token；映射写在 `lsp.clice.initialization_options` 下。

## 配置

`clice.toml` 的 `[semantic_tokens]` 配置节（或编辑器 `initializationOptions` 中的同名键）包含词法 Token 的开关和两张重命名表。这些名称只在编辑器连接时公布一次：它们取自编辑器的设置和第一个工作区文件夹的 `clice.toml`，修改后需要重启服务器。详见[配置指南](../guide/configuration.md#semantic-tokens)。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/options -->

<!-- BEGIN CAPABILITY: supported -->

**词法 Token**

关闭 `lexical` 后，只发送编辑器自身语法无法区分的内容

关键字、字面量、注释、内置运算符和指令交给编辑器的语法着色。名称、模板尖括号和声明符运算符仍会发送，未选中分支中的代码也会作为 `plain` Token 发送：语法赋予的颜色保持不变，只是被淡化。

```case
tests/semantic_tokens/options/01_lexical_tokens
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**种类名称**

`kinds` 重命名服务器公布的 Token 种类

赋予同一名称的两个种类合并为该名称，重命名为 `""` 的种类不再发送，保留编辑器自身的颜色。这些名称只在编辑器连接时公布一次，修改后需要重启。

```case
tests/semantic_tokens/options/02_kind_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**修饰符名称**

`modifiers` 重命名服务器公布的 Token 修饰符

重命名为 `""` 的修饰符永远不会被设置。与种类一样，这些名称只在编辑器连接时公布一次。

```case
tests/semantic_tokens/options/03_modifier_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 词法 Token

直接从 Token 流本身推导出的种类，不依赖 AST。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/lexical -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

行注释、块注释和文档注释均标记为注释 Token，文档注释还带有 `documentation` 修饰符

```case
tests/semantic_tokens/lexical/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**字面量**

数字、字符和字符串均标记为字面量 Token

```case
tests/semantic_tokens/lexical/02_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关键字**

运算符的替代拼写标记为带 `operator` 修饰符的关键字，上下文说明符仍标记为关键字 Token

```case
tests/semantic_tokens/lexical/03_keywords
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基本类型**

内置类型使用独立的 Token 种类，而非普通的 `keyword`

```case
tests/semantic_tokens/lexical/04_primitive_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**字面量前缀和后缀**

字面量的前缀和后缀是独立的 Token，带有 `affix` 修饰符

它们保留所属字面量的种类；用户定义字面量的后缀还带有 `userDefined`。

```case
tests/semantic_tokens/lexical/05_literal_affixes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**转义序列**

字面量中的转义序列是 `escape` Token

```case
tests/semantic_tokens/lexical/06_escape_sequences
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1421 -->

**声明符与运算符的区分**

声明符中的 `*`、`&` 和 `&&` 带有 `declarator` 修饰符，与表达式中的同名运算符区分开

```case
tests/semantic_tokens/lexical/07_declarator_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**括号的 Token 类型**

模板实参和模板参数两侧的尖括号是 `bracket` Token

这样它们就能与编辑器语法误认作的比较运算符区分开。圆括号、方括号和花括号由编辑器自行配对，不生成 Token。

```case
tests/semantic_tokens/lexical/08_brackets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2209 -->

**属性名称**

属性名称是 `attribute` Token，其实参保留各自的种类

```case
tests/semantic_tokens/lexical/09_attributes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**格式字符串**

格式字符串中的占位符是 `formatSpecifier` Token

传给函数格式字符串形参的字符串会拆分为文本和占位符。这类形参指 `basic_format_string` 形参（`std::format`、`std::print` 和 {fmt} 都这样声明），或由 `format(printf, …)` 或 `format(scanf, …)` 属性指明的那个参数。前一种风格的占位符如 `{}` 和 `{:>8.2f}`，printf 风格的如 `%d` 和 `%-8.3f`，scanf 风格的如 `%ms` 和 `%15[^,\n]`，原始字符串也不例外。转义的花括号和 `%%` 是转义序列；没有任何函数当作格式字符串读取的字符串则是普通文本。

```case
tests/semantic_tokens/lexical/12_format_strings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文档注释**

文档注释中的命令是关键字，命令所说明的名称是参数

在 `///` 或 `/** */` 注释中，`\param`、`@brief`、`@return` 这样的命令是 `keyword`，`@param` 之后的名称是 `parameter`，两者都带有 `documentation` 修饰符；注释的其余部分仍是 `comment`。普通注释中没有命令。

```case
tests/semantic_tokens/lexical/13_doc_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**以单词拼写的运算符**

以单词拼写的运算符是带 `operator` 修饰符的关键字

`sizeof`、`alignof`、`typeid`、`new`、`delete`、具名转型以及 `and` 这类替代拼写都采用关键字的颜色，修饰符把它们与其他关键字区分开。

```case
tests/semantic_tokens/lexical/16_word_operators
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 声明

名称按其所定义或引用的声明进行分类。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/declarations -->

<!-- BEGIN CAPABILITY: supported -->

**已声明的名称**

每个已声明的名称都按其所声明的实体类别着色

```case
tests/semantic_tokens/declarations/01_declared_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间**

命名空间的定义、引用、嵌套和别名均标记为命名空间 Token

```case
tests/semantic_tokens/declarations/02_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型**

类型定义和引用保留各自的类型类别

```case
tests/semantic_tokens/declarations/03_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数和方法**

函数声明、定义和调用均标记为函数 Token

```case
tests/semantic_tokens/declarations/04_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量**

变量声明和引用保留各自的变量类别

```case
tests/semantic_tokens/declarations/05_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

类型参数是 `typeParameter`，非类型参数是 `templateParameter`，模板带有 `template`

```case
tests/semantic_tokens/declarations/06_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concept**

Concept 的定义及其在约束中的使用均标记为 Concept Token

```case
tests/semantic_tokens/declarations/07_concepts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签**

标签及其在 `goto` 中的引用均标记为标签 Token

```case
tests/semantic_tokens/declarations/08_labels
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

结构化绑定名称在定义和使用处均标记为变量 Token

起始的 `[` 特意不标记为任何 Token；仅高亮显示绑定名称本身。

```case
tests/semantic_tokens/declarations/09_structured_bindings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 引用

引用处保留其所解析到的声明的语义种类，通过语言特有的查找规则找到的名称也不例外。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/references -->

<!-- BEGIN CAPABILITY: supported clangd#122 -->

**成员初始化列表**

成员初始化列表中被初始化的名称按字段高亮

```case
tests/semantic_tokens/references/01_member_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2619 -->

**using 声明**

引入的名称保留其目标的种类

```case
tests/semantic_tokens/references/02_using_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#154 clangd#297 -->

**待决名**

待决名通过已知的主模板解析

已知模板（`Box<T>`）的待决成员解析到主模板中的声明，并保留这些声明的种类。直接以模板参数为所属类型的成员没有候选声明：它们是 `unknown`，位于 `typename` 之后时则是类型。每个待决名都带有 `dependent`。

```case
tests/semantic_tokens/references/05_dependent_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 捕获**

复制捕获和引用捕获均引用被捕获的变量；`this` 仍标记为关键字

```case
tests/semantic_tokens/references/06_lambda_captures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1390 clangd#1627 clangd#2235 clangd#2547 clangd#484 -->

**各种写法**

C++ 用较少见的写法拼出名称时，名称仍保留各自的种类

`auto` 参数是关键字，而不是模板参数；`::` 之后的 `new` 是运算符；成员指针中的类是类；重载的 `=` 的左操作数与内置 `=` 的左操作数一样被视为修改；在 `template` 之后命名的成员模板在实例化之前是 `unknown`。

```case
tests/semantic_tokens/references/14_spelling_forms
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 特殊成员

构造函数、析构函数和运算符函数均按方法标记，并带有各自的修饰符。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/special_members -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**构造函数与析构函数**

构造函数和析构函数使用带有专用修饰符的方法 Token

析构函数的完整名称，即 `~` 加上类名，是一个方法 Token。

```case
tests/semantic_tokens/special_members/01_constructors_destructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**运算符名称**

运算符函数名称中的 `operator` 仍标记为关键字，对该函数的每次调用均标记为运算符 Token

```case
tests/semantic_tokens/special_members/02_operator_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token 修饰符

修饰符补充种类未体现的信息：是否为定义、符号是否为 `static` 或 `const`、是否已弃用，以及是否来自系统头文件。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/modifiers -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义**

声明修饰符和定义修饰符用于区分这两种位置

```case
tests/semantic_tokens/modifiers/01_declaration_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**静态**

静态成员和静态局部变量带有静态修饰符

```case
tests/semantic_tokens/modifiers/02_static
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**只读**

const 值、const 成员函数以及枚举成员带有只读修饰符

只读针对的是名称所代表的对象：指向 const 的引用和 const 指针是只读的，指向 const 的指针则不是。

```case
tests/semantic_tokens/modifiers/03_readonly
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**虚与抽象**

虚成员函数和抽象类分别带有虚修饰符和抽象修饰符

```case
tests/semantic_tokens/modifiers/04_virtual_abstract
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**已弃用**

已弃用的声明及其使用处带有已弃用修饰符

```case
tests/semantic_tokens/modifiers/05_deprecated
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**默认库**

来自系统头文件的符号带有默认库修饰符

```case
tests/semantic_tokens/modifiers/06_default_library
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#352 -->

**作用域修饰符**

每个声明的名称都标明其可见范围：所在函数、所在类、所在文件或全局

```case
tests/semantic_tokens/modifiers/07_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#839 -->

**可修改的实参**

被调函数可能经由非 const 引用或指针修改的实参带有 `modification`，与赋值的目标一样

```case
tests/semantic_tokens/modifiers/08_mutable_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1521 -->

**用户定义的运算符**

对重载运算符的调用表示为带有 `userDefined` 修饰符的运算符 Token

```case
tests/semantic_tokens/modifiers/10_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**可调用的变量**

类型可被调用的变量、参数或字段带有 `callable`

函数指针、成员函数指针、Lambda，以及带有 `operator()`（无论是自身的还是基类的）的类的对象都带有它，在声明处和每个使用处都是如此。

```case
tests/semantic_tokens/modifiers/12_callable
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

类模板、别名模板、变量模板或函数模板在每个被提及的位置都带有 `template`

模板的特化也是如此；模板实参两侧的 `<` 和 `>` 是 `bracket` Token，与比较运算符区分开。

```case
tests/semantic_tokens/modifiers/13_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**控制流**

转移控制的关键字和永不返回的函数带有 `controlFlow`

`if`、`for`、`return` 以及其他用于分支、循环或退出的语句都带有它，从而与 `int`、`const` 这类声明关键字区分开。`[[noreturn]]` 函数也带有它，展开后可能离开函数的宏同样如此。

```case
tests/semantic_tokens/modifiers/14_control_flow
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 冲突与歧义

C++ 允许结构不同的实体共用一个名称。当代码中的同一个名称同时指向不同种类的实体时，没有任何单一的 Token 类型能准确表示它；这类名称会获得专用的 **conflict**（冲突）Token 类型，客户端通常以中性色显示。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/ambiguity -->

<!-- BEGIN CAPABILITY: supported -->

**有冲突的名称**

一个名称同时指代类型和函数或变量时，会显示为 `conflict`

```case
tests/semantic_tokens/ambiguity/01_conflicts
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理器

指令、头文件名和宏展开都有各自的 Token 类型，而宏定义体保留其词法类别。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/preprocessor -->

<!-- BEGIN CAPABILITY: supported -->

**预处理指令**

指令保留其指令类型，未选中分支中的代码保留其词法类别

Pragma 参数中，除数字外均不带样式。

```case
tests/semantic_tokens/preprocessor/01_directives
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件名**

引号或尖括号中的头文件名会获得头文件 Token

```case
tests/semantic_tokens/preprocessor/02_header_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未选中的分支**

未选中分支中的 Token 保留其词法类别，并带有 `inactive` 修饰符

没有自身种类的文本会变为 `plain` Token，因此即使一行只有一个 `}`，也会淡化显示。

```case
tests/semantic_tokens/preprocessor/03_inactive_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开位置与实参**

展开处的名称标记为宏，源码中的实参保留其语义，宏定义体除参数外保留其词法类别

```case
tests/semantic_tokens/preprocessor/04_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2649 -->

**对象式宏与函数式宏**

函数式宏带有 `callable`，对象式宏则不带

```case
tests/semantic_tokens/preprocessor/05_macro_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**空宏与控制流宏**

展开为空的宏带有 `empty`，可能离开函数的宏带有 `controlFlow`

这两个修饰符都标在宏定义处和每个展开处的宏名上。空宏丢弃的实参不会到达编译器，因此带有 `inactive`，与未选中分支中的代码一样。

```case
tests/semantic_tokens/preprocessor/08_empty_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**条件中的宏名**

条件所检查的名称是 `macro`，无论该宏是否已定义

无论宏是否已定义，`#ifdef`、`#ifndef`、`defined(…)`、`#undef` 所写的名称以及 `#if` 检测的名称都是宏；`defined` 和 `__has_include` 这类检查是运算符，这类检查所检测的对象不是宏，`__has_include` 检查的文件是头文件名。

```case
tests/semantic_tokens/preprocessor/09_condition_operands
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏丢弃的实参**

宏定义从未用到的参数所对应的实参带有 `inactive`

与未选中分支中的代码一样，它们不会到达编译器：展开为空的宏的所有实参都是如此，这里 `FIRST` 的第二个实参也是如此。

```case
tests/semantic_tokens/preprocessor/11_dropped_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文件开头的指令

文件开头的包含指令和宏会与文件的其余部分分开编译，其 Token 的呈现方式与其他位置相同。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/preamble -->

<!-- BEGIN CAPABILITY: supported -->

**Preamble 中的非活动区域**

文件开头各指令中未选中的分支也以相同方式显示为暗淡样式

```case
tests/semantic_tokens/preamble/01_inactive_preamble
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏定义与展开**

宏的定义及其展开都会获得宏 Token

```case
tests/semantic_tokens/preamble/02_preamble_macros
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块声明和导入中的关键字与名称都会获得 Token。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块声明**

模块声明会为上下文关键字、以点分隔的名称和私有模块片段生成 Token

```case
tests/semantic_tokens/modules/01_module_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

模块声明为分区名称生成 Token

```case
tests/semantic_tokens/modules/02_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作为标识符的 `module` 和 `import`**

上下文关键字在模块声明之外保留其语义种类

```case
tests/semantic_tokens/modules/03_contextual_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 非活动代码区域

未选中的预处理器分支内，每个 Token 都带有 `inactive` 修饰符，同时保留其词法类别，因此编辑器可以通过设置该修饰符的样式来淡化显示整个区域，同时保留原有的语法着色。非活动代码中没有自身种类的 Token（普通标识符和标点符号）以不带样式的 `plain` 种类发送，使 Token 覆盖整个区域。宏丢弃的实参（例如被编译掉的 `assert` 的实参）也以同样方式淡化显示。clice 的 VS Code 扩展默认会淡化显示这些区域；其他编辑器则直接为该修饰符设置样式（例如 Neovim 中的 `@lsp.mod.inactive`）。

- [x] 淡化显示非活动预处理器分支（[clangd#132](https://github.com/clangd/clangd/issues/132)）
- [x] 正确处理 `#elif` 链中的非活动区域边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）
- [x] 保留非活动区域内的语法高亮（[clangd#1664](https://github.com/clangd/clangd/issues/1664)）
- [x] 保持非活动区域与注释的区别（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）

### 未选中的分支

预处理器跳过的分支都会淡化显示，无论该分支由哪种指令引入。

<!-- BEGIN GENERATED ITEMS: inactive_regions/branches -->

<!-- BEGIN CAPABILITY: supported -->

**未选中的分支**

预处理器跳过的分支中的各行都会淡化显示，也包括已选中分支之后的 `#else` 分支

```case
tests/inactive_regions/branches/02_else_branch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 头文件上下文

头文件中，其[宿主（host）](./header-contexts.md)跳过的分支会淡化显示。

<!-- BEGIN GENERATED ITEMS: inactive_regions/contexts -->

<!-- BEGIN CAPABILITY: supported -->

**切换宿主**

头文件会显示所选宿主选中的分支，切换宿主时会立即更新淡化显示

```case
tests/inactive_regions/contexts/01_host_switch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 编辑

编辑条件时，淡化显示会随输入实时更新。

<!-- BEGIN GENERATED ITEMS: inactive_regions/updates -->

<!-- BEGIN CAPABILITY: supported -->

**编辑条件**

修改条件所检查的宏后，无需保存文件，淡化显示就会转移到当前被跳过的分支

```case
tests/inactive_regions/updates/01_edited_condition
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 协议支持

- [x] 全文档语义 Token（`textDocument/semanticTokens/full`）
- [x] 范围请求（`textDocument/semanticTokens/range`）：返回与该范围重叠的 Token
- [x] 增量更新（`textDocument/semanticTokens/full/delta`）：返回自编辑器持有的上一次响应以来的变化
- [x] 以 UTF-16 为单位的位置，跨行的 Token 按行拆分

## 已知限制

- 文件正在编译时（首次打开时，或其包含的头文件发生变化后），它的 Token 来自项目索引。这些 Token 涵盖编译完成后标出的所有名称，但缺少只有编译才知道的内容：待决名和 `unknown` 名称、声明符运算符、lambda 模板形参两侧的尖括号、格式字符串占位符、未选中的分支以及宏丢弃的实参。编译完成后这些内容就会出现。
- 控制流无法到达的代码，以及 `if constexpr` 中被实例化丢弃的分支，都不会淡化显示：前者需要控制流分析，后者因实例化而异（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）。
