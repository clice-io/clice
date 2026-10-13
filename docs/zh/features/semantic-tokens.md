# 语义 Token

<!-- The cards in the sections below are generated from the documented cases
     in tests/semantic_tokens/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice 使用自有的 Token 类型体系对文档中的每个 Token 进行分类。这套体系比标准 LSP Token 类型更丰富，并在 clice 的所有响应中保持一致。偏好标准 LSP 类型的客户端可以通过配置进行映射。

## 词法 Token

直接从 Token 流本身推导出的类型，不依赖 AST。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/lexical -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

行注释、块注释和文档注释均标记为注释 Token

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

运算符的替代拼写和上下文说明符仍标记为关键字 Token

```case
tests/semantic_tokens/lexical/03_keywords
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基本类型**

内置类型使用独立的 Token 类型，而非普通的 `keyword`

```case
tests/semantic_tokens/lexical/04_primitive_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**字面量前缀和后缀**

字面量前缀和后缀尚未单独标记为 Token

```case
tests/semantic_tokens/lexical/05_literal_affixes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**转义序列**

字面量中的转义序列尚未单独高亮显示

```case
tests/semantic_tokens/lexical/06_escape_sequences
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1421 -->

**声明符与运算符的区分**

声明符中的运算符和表达式中的运算符尚未使用不同的 Token 类型

```case
tests/semantic_tokens/lexical/07_declarator_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**括号的 Token 类型**

匹配的括号尚未使用各对括号专属的 Token 类型

```case
tests/semantic_tokens/lexical/08_brackets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2209 -->

**属性名称**

属性名称及其表达式目前尚不获得语义 Token

```case
tests/semantic_tokens/lexical/09_attributes
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

模板参数标记为类型或变量 Token，声明的模板带有 `templated` 修饰符

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

<!-- BEGIN CAPABILITY: partial clangd#154 clangd#297 -->

**待决名**

待决名通过已知的主模板解析

已知模板（`Box<T>`）的待决成员解析到主模板中的声明，并保留这些声明的种类。直接以模板参数为所属类型的成员没有候选声明，目前不获得 Token；对此类名称的启发式高亮仍有待解决。

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

<!-- END GENERATED ITEMS -->

## 特殊成员

构造函数、析构函数和运算符函数均按方法标记，并带有各自的修饰符。

<!-- BEGIN GENERATED ITEMS: semantic_tokens/special_members -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**构造函数与析构函数**

构造函数和析构函数使用带有专用修饰符的方法 Token

析构函数名称标注为两个 Token：`~` 带有方法类型以及声明／定义修饰符，其后的类名仍是对类的引用。

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

修饰符补充 Token 类型未体现的信息：是否为定义、符号是否为 `static` 或 `const`、是否已弃用，以及是否来自系统头文件。

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

目前只读属性根据值判断：指向 const 对象的指针也视为只读，即使指针本身可以改变。

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

每个名称都标明其可见范围：所在函数、所在类、所在文件或全局

```case
tests/semantic_tokens/modifiers/07_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#839 -->

**可修改的实参**

若被调函数可能经由非 const 引用或指针修改某个实参，该实参会带有修饰符

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

未归类的文本会变为不带样式的 `identifier` Token，因此即使一行只有一个 `}`，也会淡化显示。

```case
tests/semantic_tokens/preprocessor/03_inactive_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开位置与实参**

展开处的名称标记为宏，源码中的实参保留其语义，宏定义体保留其词法类别

```case
tests/semantic_tokens/preprocessor/04_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2649 -->

**对象式宏与函数式宏**

对象式宏和函数式宏目前尚未使用不同的 Token 类型

```case
tests/semantic_tokens/preprocessor/05_macro_kinds
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

未选中的预处理器分支内，每个 Token 都带有 `inactive` 修饰符，同时保留其词法类别，因此编辑器可以通过设置该修饰符的样式来淡化显示整个区域，同时保留原有的语法着色。非活动代码中没有分类的 Token（普通标识符和标点符号）以不带样式的 `identifier` 类型输出，使 Token 覆盖整个区域。clice 的 VS Code 扩展默认会淡化显示这些区域；其他编辑器则直接为该修饰符设置样式（例如 Neovim 中的 `@lsp.mod.inactive`）。

- [x] 淡化显示非活动预处理器分支（[clangd#132](https://github.com/clangd/clangd/issues/132)）
- [x] 正确处理 `#elif` 链中的非活动区域边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）
- [x] 保留非活动区域内的语法高亮（[clangd#1664](https://github.com/clangd/clangd/issues/1664)）
- [x] 保持非活动区域与注释的区别（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）
- [ ] 淡化显示不可达代码（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）

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

## 其他已知不足

已整理但尚无测试用例的问题：

- [ ] `auto` 参数不得高亮为模板类型参数
      （[clangd#1390](https://github.com/clangd/clangd/issues/1390)）
- [ ] 成员指针中的嵌套名说明符（nested name specifier）应获得 Token
      （[clangd#2235](https://github.com/clangd/clangd/issues/2235)）
- [ ] `::new` 中的 `new` 关键字应保持高亮
      （[clangd#1627](https://github.com/clangd/clangd/issues/1627)）
- [ ] 当协程返回类型为模板时，`co_yield` / `co_await` 会丢失高亮（[clangd#2437](https://github.com/clangd/clangd/issues/2437)）
- [ ] Token 修饰符应应用于重载运算符的操作数（[clangd#2547](https://github.com/clangd/clangd/issues/2547)）
- [ ] 依赖模板名（`obj.template get<int>()`）、通过 `using` 从依赖基类引入的成员，
      以及重载集合种类混杂的依赖名（[clangd#484](https://github.com/clangd/clangd/issues/484)、
      [clangd#686](https://github.com/clangd/clangd/issues/686)、
      [clangd#1057](https://github.com/clangd/clangd/issues/1057)）

## 格式字符串高亮

- [ ] `std::format` / `std::print` 占位符高亮（[clangd#1709](https://github.com/clangd/clangd/issues/1709)）
- [ ] 将无效的格式说明符高亮显示为错误

## 协议支持

- [x] 全文档语义 Token（`textDocument/semanticTokens/full`）
- [x] 以 UTF-16 码元为单位、采用差分编码的 Token 位置
- [ ] 指定范围的语义 Token（`textDocument/semanticTokens/range`）——仅计算可见视口内的 Token，这对大文件至关重要
- [ ] 增量更新（`textDocument/semanticTokens/full/delta`）——仅发送自上次响应以来的变化
