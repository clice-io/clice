# 语义 Token

<!-- The cards in the sections below are generated from the documented cases
     in tests/semantic_tokens/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

clice 为文档中的每个 Token 着色：名称按其声明或指代的实体着色，关键字、字面量、注释、运算符和预处理指令按其写法着色。它采用一套专为 C 和 C++ 设计的词汇，而非标准 LSP 的那一套：类型别名、模板类型参数和非类型模板参数分属三种不同的种类，`controlFlow` 这类修饰符则表明关键字、函数或宏如何影响控制流。clice 的 VS Code 和 Neovim 扩展能为这一切着色；其他编辑器则为自己认识的名称着色，可通过配置将 clice 的名称重命名为这些名称。

## 种类

| 种类                               | 标记对象                                                                                                                                                                                                                    |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `comment`                          | 注释                                                                                                                                                                                                                        |
| `string`、`character`、`number`    | 字面量，含其前缀和后缀                                                                                                                                                                                                      |
| `escape`                           | 字面量中的转义序列：`\n`、`\x41`，以及格式字符串中的 `{{`                                                                                                                                                                   |
| `formatSpecifier`                  | 格式字符串中的占位符：`{}`、`{:>8}`、`%d`                                                                                                                                                                                   |
| `keyword`                          | 关键字；起关键字作用时的 `module`、`import`、`override` 和 `final`；以单词拼写的运算符：`sizeof`、`new`、`and`；Objective-C 的 `@interface` 及其他 `@` 关键字、`@property` 的特性、方法开头的 `-` 或 `+`、`self` 和 `super` |
| `primitive`                        | 内置类型：`int`、`bool`、`char8_t`                                                                                                                                                                                          |
| `operator`                         | 运算符，无论内置还是重载                                                                                                                                                                                                    |
| `bracket`                          | 模板实参和模板参数两侧，以及 Objective-C 协议和类型实参两侧的 `<` 与 `>`                                                                                                                                                    |
| `directive`、`header`              | 预处理指令；头文件名                                                                                                                                                                                                        |
| `namespace`、`module`              | 命名空间或命名空间别名；C++20 模块名；Objective-C 分类（category）                                                                                                                                                          |
| `class`、`struct`、`union`、`enum` | 类，按声明它的关键字区分；Objective-C 类                                                                                                                                                                                    |
| `typeAlias`                        | `typedef` 或 `using` 别名                                                                                                                                                                                                   |
| `typeParameter`                    | 模板类型参数或模板模板参数                                                                                                                                                                                                  |
| `templateParameter`                | 非类型模板参数                                                                                                                                                                                                              |
| `concept`                          | 概念（concept）                                                                                                                                                                                                             |
| `interface`                        | Objective-C 协议                                                                                                                                                                                                            |
| `function`、`method`               | 自由函数；成员函数（含构造函数和析构函数），以及标在其选择器（selector）每一段上的 Objective-C 方法                                                                                                                         |
| `variable`、`parameter`、`field`   | 变量、函数形参、数据成员，以及 Objective-C 属性或实例变量                                                                                                                                                                   |
| `enumMember`、`label`              | 枚举值；标签                                                                                                                                                                                                                |
| `macro`、`macroParameter`          | 宏；函数式宏定义中的形参                                                                                                                                                                                                    |
| `attribute`                        | 属性名：`nodiscard`、`gnu::packed`                                                                                                                                                                                          |
| `unknown`                          | 只有实例化才能解析的待决名（dependent name）：`T::reset()`、`value.size()`                                                                                                                                                  |
| `conflict`                         | 同时指代不同种类实体的名称                                                                                                                                                                                                  |
| `plain`                            | 未命中分支中本身没有种类的 Token，或关闭词法 Token 后其中的任意 Token，为携带 `inactive` 而发送                                                                                                                             |

## 修饰符

与标准 LSP 修饰符同名的修饰符，在任意种类上都保持标准含义：

| 修饰符           | 含义                                                                            |
| ---------------- | ------------------------------------------------------------------------------- |
| `declaration`    | 声明该实体而不定义它                                                            |
| `definition`     | 定义该实体                                                                      |
| `readonly`       | 无法通过该名称修改对象：常量、指向 const 的引用、const 指针、枚举值、const 方法 |
| `static`         | 静态成员或静态局部变量                                                          |
| `deprecated`     | 实体已弃用                                                                      |
| `abstract`       | 纯虚方法，或含有纯虚方法的类                                                    |
| `defaultLibrary` | 声明于系统头文件中，或由编译器内置                                              |
| `documentation`  | 位于文档注释内部                                                                |
| `modification`   | 实体在此处被修改：赋值、自增、作为成员被初始化，或经引用或地址传给非 const 参数 |

声明引入的名称会标明其可见范围：`local`（所在函数或代码块内）、`member`（所在类内）、`internal`（所在翻译单元内，由 `static` 或无名命名空间限定）或 `global`。

其余修饰符为 clice 自有，其中部分修饰符在不同种类上各有特定含义：

| 修饰符                      | 作用于                           | 含义                                                                                |
| --------------------------- | -------------------------------- | ----------------------------------------------------------------------------------- |
| `template`                  | 类、别名、变量或函数             | 模板，或模板的特化                                                                  |
| `dependent`                 | 任意名称                         | 依赖模板参数，每个 `unknown` 名称均带有它；发给 `id` 或 `Class` 的 Objective-C 消息 |
| `virtual`                   | 方法；类                         | 虚方法；多态类                                                                      |
| `constexpr`                 | 函数、变量或 `templateParameter` | 声明为 `constexpr`、`consteval` 或 `constinit`，或为非类型模板参数                  |
| `constructor`、`destructor` | 方法                             | 构造函数；析构函数                                                                  |
| `callable`                  | 宏                               | 函数式宏                                                                            |
|                             | 变量、形参或字段                 | 类型可调用：函数指针、Lambda、带有 `operator()` 的类                                |
| `controlFlow`               | 关键字                           | 转移控制流：`if`、`for`、`return`、`throw`、`co_await`、`case`                      |
|                             | 函数                             | `[[noreturn]]`                                                                      |
|                             | 宏                               | 展开结果可能离开当前函数：`return`、`break`、`goto`、`throw`                        |
| `userDefined`               | 运算符；字面量后缀               | 调用重载运算符；用户定义字面量                                                      |
| `declarator`                | 运算符                           | 声明符中的 `*`、`&` 或 `&&`，与表达式中的同名运算符区分开                           |
| `operator`                  | 关键字                           | 运算符：`sizeof`、`new`、`static_cast`、`and`、`defined`                            |
| `literal`                   | 关键字                           | `true`、`false`、`nullptr`                                                          |
| `affix`                     | 字面量                           | 字面量的前缀或后缀：`u8`、`L`、`42u`、`1.5f`                                        |
| `empty`                     | 宏                               | 展开为空                                                                            |
| `inactive`                  | 任意 Token                       | 位于未命中的预处理分支中，或属于被宏丢弃的实参                                      |

## 编辑器

- **VS Code**：clice 扩展将每个种类和修饰符声明为对应主题着色的 TextMate 作用域，因此 `keyword.controlFlow` 自动采用主题为控制流关键字配置的颜色，`escape` 采用转义序列的颜色。
- **Neovim**：clice 插件把 clice 名称对应的 `@lsp.type.*` 和 `@lsp.mod.*` 高亮组链接到配色方案设置样式的常用高亮组，作为可被配色方案覆盖的默认值。
- **其他编辑器**（Zed、Kate、Emacs、Sublime Text）只为标准 LSP 名称着色。把 clice 的种类重命名为最接近的标准名称即可在这些编辑器中着色；重命名应写在该编辑器传给 clice 的 `initializationOptions` 中，不要写进 VS Code 也会读取的 `clice.toml`：

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

只有为 C 和 C++ 将 `semantic_tokens` 设为 `"combined"`（或 `"full"`）后，Zed 才会请求语义 Token；映射写在 `lsp.clice.initialization_options` 下。

## 配置

`[semantic_tokens]` 配置节（位于 `clice.toml` 中，也可在编辑器的 `initializationOptions` 中设置同名键）包含词法 Token 的开关和两张重命名表。这些名称只在编辑器连接时公布一次：取自编辑器设置与第一个工作区文件夹的 `clice.toml`，修改后需重启服务器才能生效。详见[配置指南](../guide/configuration.md#semantic-tokens)。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/options -->

<!-- BEGIN CAPABILITY: supported -->

**词法 Token**

关闭 `lexical` 后，只发送编辑器自身语法无法区分的内容

关键字、字面量、注释、内置运算符和预处理指令交由编辑器的语法规则着色。名称、模板尖括号和声明符运算符仍会发送，未命中分支中的代码也会作为 `plain` Token 发送：语法着色保持不变，仅做变暗处理。

```case
tests/semantic_tokens/options/01_lexical_tokens
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**种类名称**

`kinds` 重命名服务器公布的 Token 种类

改为同一名称的两个种类会合并为该名称；重命名为 `""` 的种类不再发送，保留编辑器自身的颜色。这些名称只在编辑器连接时公布一次，修改后需要重启服务器。

```case
tests/semantic_tokens/options/02_kind_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**修饰符名称**

`modifiers` 重命名服务器公布的 Token 修饰符

重命名为 `""` 的修饰符不会再设置。与种类一样，这些名称只在编辑器连接时公布一次。

```case
tests/semantic_tokens/options/03_modifier_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 词法 Token

直接从 Token 流推导出的种类，不依赖 AST。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/lexical -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

行注释、块注释和文档注释均获得注释 Token，文档注释带有 `documentation` 修饰符

```case
tests/semantic_tokens/lexical/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**字面量**

数字、字符和字符串均获得字面量 Token

```case
tests/semantic_tokens/lexical/02_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关键字**

运算符的替代拼写是带 `operator` 修饰符的关键字，上下文说明符保留关键字 Token

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

**字面量前缀与后缀**

字面量的前缀和后缀各自成为独立的 Token，带有 `affix` 修饰符

它们保留所属字面量的种类；用户定义字面量的后缀还带有 `userDefined`。

```case
tests/semantic_tokens/lexical/05_literal_affixes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**转义序列**

字面量内部的转义序列为 `escape` Token

```case
tests/semantic_tokens/lexical/06_escape_sequences
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1421 -->

**声明符与表达式运算符区分**

声明符中的 `*`、`&` 和 `&&` 带有 `declarator` 修饰符，与表达式中的同名运算符区分开

```case
tests/semantic_tokens/lexical/07_declarator_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**尖括号 Token 类型**

模板实参和模板参数两侧的尖括号为 `bracket` Token

这使它们能与编辑器语法规则误判的比较运算符区分开。圆括号、方括号和花括号由编辑器自行配对，不生成语义 Token。

```case
tests/semantic_tokens/lexical/08_brackets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2209 -->

**属性名称**

属性名称为 `attribute` Token，其实参保留各自的种类

```case
tests/semantic_tokens/lexical/09_attributes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**格式字符串**

格式字符串中的占位符为 `formatSpecifier` Token

传给函数格式字符串位置的字符串——即 `basic_format_string` 形参（`std::format`、`std::print` 和 {fmt} 均如此声明），或由 `format(printf, …)`、`format(scanf, …)` 属性指定的实参——会拆分为普通文本与占位符：第一种风格下的 `{}` 和 `{:>8.2f}`，printf 风格下的 `%d` 和 `%-8.3f`，scanf 风格下的 `%ms` 和 `%15[^,\n]`，原始字符串也不例外。转义花括号与 `%%` 标为转义序列；未被任何函数当作格式字符串读取的字符串保持为普通文本。

```case
tests/semantic_tokens/lexical/12_format_strings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文档注释**

文档注释中的命令标为关键字，命令所说明的名称标为形参

在 `///` 或 `/** */` 注释内部，`\param`、`@brief` 或 `@return` 这类命令标为 `keyword`，`@param` 后的名称标为 `parameter`，二者均带有 `documentation` 修饰符；注释的其余部分仍为 `comment`。普通注释不识别命令。

```case
tests/semantic_tokens/lexical/13_doc_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单词形式的运算符**

`sizeof`、`new` 和 `static_cast` 等以单词拼写的运算符是带 `operator` 修饰符的关键字

`sizeof`、`alignof`、`typeid`、`new`、`delete`、具名类型转换以及 `and` 等替代拼写都采用关键字颜色，由该修饰符将它们与其他关键字区分开。`co_await` 会挂起协程，带的是 `controlFlow`。

```case
tests/semantic_tokens/lexical/16_word_operators
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 声明

名称按其所定义或引用的声明分类。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/declarations -->

<!-- BEGIN CAPABILITY: supported -->

**已声明的名称**

每个已声明的名称按其声明的实体着色

```case
tests/semantic_tokens/declarations/01_declared_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间**

命名空间的定义、引用、嵌套和别名均获得命名空间 Token

```case
tests/semantic_tokens/declarations/02_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型**

类型定义与引用保留各自的类型种类

```case
tests/semantic_tokens/declarations/03_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数与方法**

函数的声明、定义和调用均获得函数 Token

```case
tests/semantic_tokens/declarations/04_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量**

变量声明与引用保留各自的变量种类

```case
tests/semantic_tokens/declarations/05_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

类型参数标为 `typeParameter`，非类型参数标为 `templateParameter`，模板本身带有 `template`

```case
tests/semantic_tokens/declarations/06_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**概念（Concept）**

概念的定义及其在约束中的使用均获得 Concept Token

```case
tests/semantic_tokens/declarations/07_concepts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签**

标签及其在 `goto` 中的引用均获得标签 Token

```case
tests/semantic_tokens/declarations/08_labels
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

结构化绑定名称在定义处和使用处均获得变量 Token

开头的 `[` 特意不附加任何 Token，仅高亮绑定名称本身。

```case
tests/semantic_tokens/declarations/09_structured_bindings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 引用

引用位置保留其解析到的声明的语义种类，经由语言特有的查找规则解析出的名称也不例外。

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

引入的名称保留其目标声明的种类

```case
tests/semantic_tokens/references/02_using_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#154 clangd#297 -->

**待决名**

待决名通过已知的主模板解析

已知模板（`Box<T>`）的待决成员会解析到主模板中的声明，并保留各自的种类。裸模板参数的成员没有候选声明：一般标为 `unknown`，跟在 `typename` 之后时标为类型。每个待决名均带有 `dependent`。

```case
tests/semantic_tokens/references/05_dependent_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 捕获**

按值捕获和按引用捕获均引用被捕获的变量；`this` 仍标为关键字

```case
tests/semantic_tokens/references/06_lambda_captures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1390 clangd#1627 clangd#2235 clangd#2547 clangd#484 -->

**特殊书写形式**

C++ 中较少见的书写形式下，名称依然保留各自的种类

`auto` 形参是关键字而非模板参数；`new` 出现在 `::` 之后时是运算符；成员指针中的类名标为类；重载 `=` 的左操作数与内置赋值一样标为被修改；紧跟 `template` 关键字指明的成员模板在实例化之前标为 `unknown`。

```case
tests/semantic_tokens/references/14_spelling_forms
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 特殊成员

构造函数、析构函数和运算符函数按方法着色，并带有各自的专属修饰符。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/special_members -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**构造函数与析构函数**

构造函数和析构函数使用带有专属修饰符的方法 Token

析构函数的完整名称（`~` 连同类名的整体）作为一个方法 Token。

```case
tests/semantic_tokens/special_members/01_constructors_destructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**运算符函数名称**

运算符函数名中的 `operator` 仍标为关键字，每次对它的调用均标为运算符 Token

```case
tests/semantic_tokens/special_members/02_operator_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token 修饰符

修饰符补充种类之外的信息：是否为定义、是否为 `static` 或 `const` 符号、是否已弃用，以及是否来自系统头文件。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/modifiers -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义**

声明修饰符和定义修饰符用于区分声明处与定义处

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

const 值、const 方法以及枚举成员带有只读修饰符

只读针对的是名称所代表的对象本身：指向 const 的引用和 const 指针是只读的，指向 const 的指针则不是。

```case
tests/semantic_tokens/modifiers/03_readonly
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**虚与抽象**

虚方法和抽象类分别带有虚修饰符或抽象修饰符

```case
tests/semantic_tokens/modifiers/04_virtual_abstract
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**已弃用**

已弃用的声明及其使用处均带有已弃用修饰符

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

已声明的名称标明其可见范围：所在函数、所在类、所在文件或全局

```case
tests/semantic_tokens/modifiers/07_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#839 -->

**可修改的实参**

被调函数可能通过非 const 引用或非 const 指针修改的实参带有 `modification`，与赋值目标一致

```case
tests/semantic_tokens/modifiers/08_mutable_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1521 -->

**用户定义运算符**

对重载运算符的调用标为带 `userDefined` 修饰符的运算符 Token

```case
tests/semantic_tokens/modifiers/10_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**可调用变量**

类型可调用的变量、形参或字段带有 `callable`

函数指针、成员函数指针、Lambda，以及带有 `operator()`（无论来自自身还是基类）的类对象，在声明处和每处使用时都带有该修饰符。

```case
tests/semantic_tokens/modifiers/12_callable
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

类模板、别名模板、变量模板或函数模板在每处被提及时都带有 `template`

模板的特化同样如此；模板实参两侧的 `<` 和 `>` 标为 `bracket` Token，与比较运算符区分开。

```case
tests/semantic_tokens/modifiers/13_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**控制流**

转移控制流的关键字和永不返回的函数带有 `controlFlow`

`if`、`for`、`return` 等用于分支、循环或跳出的语句关键字均带有该修饰符，与 `int`、`const` 等声明关键字区分开。`[[noreturn]]` 函数也带有它，展开后可能离开当前函数的宏同样如此。

```case
tests/semantic_tokens/modifiers/14_control_flow
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 冲突与歧义

C++ 允许结构不同的实体共用同一名称。当代码中写出的单个名称同时指向不同种类的实体时，没有任何单一的 Token 类型是准确的；这类名称会获得专用的 **conflict** Token 类型，客户端通常以中性色显示。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/ambiguity -->

<!-- BEGIN CAPABILITY: supported -->

**有冲突的名称**

同时指代类型与函数或变量的名称标为 `conflict`

```case
tests/semantic_tokens/ambiguity/01_conflicts
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 预处理器

预处理指令、头文件名和宏展开各自获得对应的 Token，宏定义体则保留其词法种类。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/preprocessor -->

<!-- BEGIN CAPABILITY: supported -->

**预处理指令**

预处理指令保留指令种类，未命中分支中的代码保留各自的词法种类

除数字外的 pragma 实参保持为普通文本。

```case
tests/semantic_tokens/preprocessor/01_directives
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件名**

双引号和尖括号中的头文件名均获得头文件 Token

```case
tests/semantic_tokens/preprocessor/02_header_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**未命中的分支**

未命中分支中的 Token 保留各自的词法种类，并带有 `inactive` 修饰符

本身没有种类的文本会转为 `plain` Token，因此即便单独一行的 `}` 也会变暗。

```case
tests/semantic_tokens/preprocessor/03_inactive_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏展开位置与实参**

展开处的宏名标为宏，写出的实参保留各自的语义，宏定义体除形参外保留词法种类

```case
tests/semantic_tokens/preprocessor/04_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2649 -->

**对象式宏与函数式宏**

函数式宏带有 `callable`，对象式宏不带

```case
tests/semantic_tokens/preprocessor/05_macro_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**空宏与控制流宏**

展开为空的宏带有 `empty`，展开后可能离开当前函数的宏带有 `controlFlow`

这两个修饰符在宏的定义处和每次展开处都会标在宏名上。被空宏丢弃的实参不会到达编译器，与未命中分支中的代码一样带有 `inactive`。

```case
tests/semantic_tokens/preprocessor/08_empty_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**条件指令中的宏名**

条件指令所检测的名称无论是否已定义均标为 `macro`

`#ifdef`、`#ifndef`、`defined(…)`、`#undef` 以及 `#if` 所检测的名称，无论是否已定义都指代宏；`defined` 与 `__has_include` 等检查本身标为运算符，这类检查所检测的对象不是宏，而 `__has_include` 检测的文件标为头文件名。

```case
tests/semantic_tokens/preprocessor/09_condition_operands
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**被宏丢弃的实参**

宏定义体中从未用到的形参所对应的实参带有 `inactive`

与未命中分支中的代码一样，这些实参永远不会到达编译器：展开为空的宏会丢弃全部实参，此处的 `FIRST` 则丢弃第二个实参。

```case
tests/semantic_tokens/preprocessor/11_dropped_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文件头部的指令区

文件开头的包含指令和宏会与文件其余部分分开编译，但其中的 Token 与其他位置表现完全一致。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/preamble -->

<!-- BEGIN CAPABILITY: supported -->

**非活动的 Preamble 区域**

文件头部指令区中未命中的分支同样会变暗

```case
tests/semantic_tokens/preamble/01_inactive_preamble
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**宏定义与展开**

宏的定义处与各处展开均获得宏 Token

```case
tests/semantic_tokens/preamble/02_preamble_macros
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

模块声明和导入语句中的关键字与名称各自获得对应的 Token。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块声明**

模块声明为上下文关键字、点分模块名和私有模块片段生成 Token

```case
tests/semantic_tokens/modules/01_module_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

模块声明为分区名生成 Token

```case
tests/semantic_tokens/modules/02_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作为标识符的 `module` 与 `import`**

上下文关键字在模块声明之外保留各自的语义种类

```case
tests/semantic_tokens/modules/03_contextual_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 语言支持

C 和 Objective-C 源文件使用与 C++ 相同的种类体系着色：Objective-C 实体采用上表为其指定的种类。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/languages -->

<!-- BEGIN CAPABILITY: supported -->

**Objective-C 源文件**

Objective-C 的类、协议、分类、方法、属性和实例变量各按其所属实体着色

选择器的每一段都按其所指代的方法着色；`@interface`、`@property` 等 `@` 关键字按关键字着色，`self` 和 `super` 与 C++ 中的 `this` 一样着色，`@"text"` 按字符串着色。

```case
tests/semantic_tokens/languages/02_objective_c
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 非活动代码区域

未命中的预处理器分支内，每个 Token 都带有 `inactive` 修饰符并保留其词法种类，因此编辑器只需为该修饰符设置样式即可将该区域变暗，同时保留底层的语法颜色。死代码中本身没有种类的 Token（普通标识符与标点符号）以不带样式的 `plain` 种类发送，使整个区域都有 Token 覆盖。被宏丢弃的实参（例如编译期裁掉的 `assert` 实参）也以同样方式变暗。clice 的 VS Code 扩展开箱即支持将这类区域变暗显示；其他编辑器可直接为该修饰符设置样式（如 Neovim 中的 `@lsp.mod.inactive`）。

- 未命中的预处理器分支会变暗显示（[clangd#132](https://github.com/clangd/clangd/issues/132)），并在 `#elif` 链中保持准确的边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）。
- 区域内部保留语法着色（[clangd#1664](https://github.com/clangd/clangd/issues/1664)），且与注释保持区分（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）。

预处理器跳过的分支无论由哪种指令引入都会变暗显示。在头文件中，变暗的是其[宿主](./header-contexts.md)跳过的分支。由该文件其他上下文编译过的代码在变暗的同时仍保留语义种类。修改条件时，变暗区域会随输入实时更新。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/inactive -->

<!-- BEGIN CAPABILITY: supported -->

**未命中的分支**

预处理器跳过的分支各行均会变暗，已命中分支之后的 `#else` 分支也不例外

```case
tests/semantic_tokens/inactive/02_else_branch
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**切换宿主**

头文件按当前所选宿主展示命中的分支，切换宿主后变暗区域立即更新

```case
tests/semantic_tokens/inactive/04_host_switch
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**编辑条件**

修改条件所检测的宏后，无需保存文件，变暗效果就会转移到当前被跳过的分支

```case
tests/semantic_tokens/inactive/08_edited_condition
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 协议支持

- 全文档语义 Token（`textDocument/semanticTokens/full`）。
- 范围请求（`textDocument/semanticTokens/range`）：返回与指定范围重叠的 Token。
- 增量更新（`textDocument/semanticTokens/full/delta`）：返回自编辑器当前持有的响应以来的差异。
- 默认使用 UTF-16 位置编码，编辑器支持时可选用 UTF-8 或 UTF-32；跨行 Token 按行拆分。

## 已知限制

- 文件正在编译期间——首次打开时，或其包含的头文件发生更改后——其 Token 先由项目索引提供。这些 Token 能标出编译完成后所能标出的一切具名实体，但缺少只有完整编译才能得知的信息：待决名与 `unknown` 名称、声明符运算符、Lambda 模板形参两侧的尖括号、格式字符串占位符、未命中的分支，以及被宏丢弃的实参。编译完成后这些内容便会出现。
- 控制流不可达的代码，以及 `if constexpr` 中在某次实例化时被丢弃的分支，均不会变暗显示：前者需要控制流分析，后者在不同实例化之间各不相同（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）。
