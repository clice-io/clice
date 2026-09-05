# 语义 Token

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 使用自有的 Token 类型体系对文档中的每个 Token 进行分类。这套体系比标准 LSP Token 类型更丰富，并在 clice 的所有响应中保持一致。偏好标准 LSP 类型的客户端可以通过配置进行映射。

## 词法 Token

直接从 Token 流本身推导出的类型，不依赖 AST。

<!-- BEGIN GENERATED ITEMS: lexical_tokens -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

行注释、块注释和文档注释均标记为注释 Token

```snap
tests/snap/semantic_tokens/lexical_tokens/01_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**字面量**

数字、字符和字符串均标记为字面量 Token

```snap
tests/snap/semantic_tokens/lexical_tokens/02_literals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关键字**

运算符的替代拼写和上下文说明符仍标记为关键字 Token

```snap
tests/snap/semantic_tokens/lexical_tokens/03_keywords.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预处理指令**

`#if` 指令链保留指令类型；未启用的分支保留词法类型；pragma 参数保持普通文本样式

```snap
tests/snap/semantic_tokens/lexical_tokens/04_directives.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**非活动区域**

未选中分支中的 Token 保留其词法类型，并带有 `inactive` 修饰符；未分类的 Token 则归为普通的 `identifier` 类型以承载该修饰符，因此即使某行只有一个 `}`，也会显示为暗淡样式

```snap
tests/snap/semantic_tokens/lexical_tokens/05_inactive_regions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**头文件名**

包含指令中用引号或尖括号括起的文件名均标记为字符串 Token

```snap
tests/snap/semantic_tokens/lexical_tokens/06_include_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Preamble 中的非活动区域**

文件开头各指令中未选中的分支也以相同方式显示为暗淡样式

```snap
tests/snap/semantic_tokens/lexical_tokens/07_inactive_preamble.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**字面量前缀和后缀**

字面量的前缀、后缀和分隔符尚未标记为独立的 Token

```snap
tests/snap/semantic_tokens/lexical_tokens/08_literal_affixes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**转义序列**

字面量中的转义序列尚未单独高亮显示

```snap
tests/snap/semantic_tokens/lexical_tokens/09_escape_sequences.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1421 -->

**声明符与运算符的区分**

声明符中的运算符和表达式中的运算符尚未使用不同的 Token 类型

```snap
tests/snap/semantic_tokens/lexical_tokens/10_declarator_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基本类型的 Token 类型**

内置类型使用独立的 Token 类型，而非普通的 `keyword`

```snap
tests/snap/semantic_tokens/lexical_tokens/11_primitive_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**括号的 Token 类型**

匹配的括号尚未使用各对括号专属的 Token 类型

```snap
tests/snap/semantic_tokens/lexical_tokens/12_bracket_pairs.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 声明

名称按其所定义或引用的声明进行分类。

<!-- BEGIN GENERATED ITEMS: declarations -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间**

命名空间的定义、引用、嵌套和别名均标记为命名空间 Token

```snap
tests/snap/semantic_tokens/declarations/01_namespaces.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型**

类型定义和引用保留各自的类型类别

```snap
tests/snap/semantic_tokens/declarations/02_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**函数和方法**

函数声明、定义和调用均标记为函数 Token

```snap
tests/snap/semantic_tokens/declarations/03_functions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量**

变量声明和引用保留各自的变量类别

```snap
tests/snap/semantic_tokens/declarations/04_variables.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板**

模板参数标记为类型或变量类别，模板名称带有 `templated` 修饰符

```snap
tests/snap/semantic_tokens/declarations/05_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concept**

Concept 的定义及其在约束中的使用均标记为 Concept Token

```snap
tests/snap/semantic_tokens/declarations/06_concepts.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**标签**

标签及其在 `goto` 中的引用均标记为标签 Token

```snap
tests/snap/semantic_tokens/declarations/07_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**结构化绑定（structured bindings）**

结构化绑定名称在定义和使用处均标记为变量 Token

起始的 `[` 特意不标记为任何 Token；仅高亮显示绑定名称本身。

```snap
tests/snap/semantic_tokens/declarations/08_structured_bindings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#868 -->

**Lambda 初始化捕获**

Lambda 初始化捕获标记为变量 Token

```snap
tests/snap/semantic_tokens/declarations/09_lambda_init_capture.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导指引（deduction guides）**

推导指引及其所引导的模板均标记为类型 Token

```snap
tests/snap/semantic_tokens/declarations/10_deduction_guides.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#316 -->

**显式实例化**

类的显式实例化会高亮显示模板名称和显式写出的实参

```snap
tests/snap/semantic_tokens/declarations/11_explicit_instantiation_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**变量模板**

变量模板的声明、定义和特化均标记为变量 Token

```snap
tests/snap/semantic_tokens/declarations/12_variable_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类外成员定义**

限定名称保留方法类别和修饰符

```snap
tests/snap/semantic_tokens/declarations/13_out_of_line_methods.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**别名模板**

别名名称带有类型类别和 `templated` 修饰符

```snap
tests/snap/semantic_tokens/declarations/14_alias_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板模板参数**

模板模板参数（template-template parameters）在声明和使用处均获得类型 Token

```snap
tests/snap/semantic_tokens/declarations/15_template_template_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**友元声明**

友元声明中的名称解析到其目标；内联友元标记为定义

```snap
tests/snap/semantic_tokens/declarations/16_friend_declarations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**函数显式实例化指令**

函数显式实例化指令中的标识符仍不高亮

```snap
tests/snap/semantic_tokens/declarations/17_explicit_instantiation_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**变量显式实例化指令**

变量显式实例化指令中的标识符仍不高亮

```snap
tests/snap/semantic_tokens/declarations/18_explicit_instantiation_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**显式实例化的成员体**

待决名（dependent names）按实际解析结果高亮：各实例化中的种类一致时，保留所有实例化共有的修饰符；种类不一致时，标记为冲突

```snap
tests/snap/semantic_tokens/declarations/19_explicit_instantiation_member_bodies.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 引用

引用处保留其所解析到的声明的语义种类，通过语言特有的查找规则找到的名称也不例外。

<!-- BEGIN GENERATED ITEMS: references -->

<!-- BEGIN CAPABILITY: supported clangd#122 -->

**成员初始化列表**

成员初始化列表中被初始化的名称按字段高亮

```snap
tests/snap/semantic_tokens/references/01_member_init_list.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2619 -->

**using 声明**

引入的名称保留其目标的种类

```snap
tests/snap/semantic_tokens/references/02_using_declarations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#213 -->

**`sizeof...`**

参数包保留其类型参数 Token

```snap
tests/snap/semantic_tokens/references/03_sizeof_pack.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1283 -->

**`using enum`**

using 声明在 using 处高亮枚举名称

```snap
tests/snap/semantic_tokens/references/04_using_enum.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#154 clangd#297 -->

**待决名**

待决名通过已知的主模板解析

已知模板（`Box<T>`）的待决成员解析到主模板中的声明，并保留这些声明的种类。直接以模板参数为所属类型的成员没有候选声明，目前不获得 Token；对此类名称的启发式高亮仍有待解决。

```snap
tests/snap/semantic_tokens/references/05_dependent_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 捕获**

按复制捕获和按引用捕获都引用被捕获的变量；`this` 仍作为关键字

```snap
tests/snap/semantic_tokens/references/06_lambda_captures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**范围 for 循环**

范围 for 循环变量在定义和使用处均保留变量 Token

```snap
tests/snap/semantic_tokens/references/07_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**枚举底层类型**

枚举底层类型的引用保留其类型种类

```snap
tests/snap/semantic_tokens/references/08_enum_base.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**待决 using 声明**

待决 using 声明仍不高亮

引入的名称及其使用处目前均不获得 Token；预留的待决名修饰符尚未输出。

```snap
tests/snap/semantic_tokens/references/09_dependent_using.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块

<!-- BEGIN GENERATED ITEMS: modules -->

<!-- BEGIN CAPABILITY: supported -->

**模块声明**

模块声明为上下文关键字、以点分隔的名称和私有模块片段生成 Token

```snap
tests/snap/semantic_tokens/modules/01_modules.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

模块声明为分区名称生成 Token

```snap
tests/snap/semantic_tokens/modules/02_module_partition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作为标识符的 `module` 和 `import`**

上下文关键字在模块声明之外保留其语义种类

```snap
tests/snap/semantic_tokens/modules/03_module_keyword_identifier.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token 修饰符

<!-- BEGIN GENERATED ITEMS: token_modifiers -->

<!-- BEGIN CAPABILITY: supported -->

**声明与定义**

声明修饰符和定义修饰符用于区分这两种位置

```snap
tests/snap/semantic_tokens/token_modifiers/01_decl_def_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**静态**

静态成员和静态局部变量带有静态修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/02_static_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**只读**

const 值、const 成员函数以及枚举成员带有只读修饰符

目前只读属性根据值判断：指向 const 对象的指针也视为只读，即使指针本身可以改变。

```snap
tests/snap/semantic_tokens/token_modifiers/03_readonly_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**虚与抽象**

虚成员函数和抽象类分别带有虚修饰符和抽象修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/04_virtual_abstract.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**已弃用**

已弃用的声明及其使用处带有已弃用修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/05_deprecated_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**默认库**

来自系统头文件的符号带有默认库修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/06_default_library.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#352 -->

**作用域修饰符**

符号尚不带有函数、类、文件或全局作用域修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/07_scope_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#839 -->

**可变引用和指针**

可变引用和指针实参尚不带有修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/08_mutable_reference.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**推导**

推导出的类型尚不带有专用修饰符

```snap
tests/snap/semantic_tokens/token_modifiers/09_deduced_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1521 -->

**用户定义的运算符**

重载运算符目前尚未与内置运算符区分

```snap
tests/snap/semantic_tokens/token_modifiers/10_user_defined_operator.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 冲突与歧义

C++ 允许结构不同的实体共用一个名称。当代码中的同一个名称同时指向不同种类的实体时，没有任何单一的 Token 类型能准确表示它；这类名称会获得专用的 **conflict**（冲突）Token 类型，客户端通常以中性色显示。

<!-- BEGIN GENERATED ITEMS: conflict_ambiguity -->

<!-- BEGIN CAPABILITY: supported -->

**类型与函数**

同时指代类型和函数的名称标注为 `conflict`

```snap
tests/snap/semantic_tokens/conflict_ambiguity/01_conflict_using.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类型与变量**

同时指代类型和变量的名称标注为 `conflict`

```snap
tests/snap/semantic_tokens/conflict_ambiguity/02_conflict_type_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**同类重载集**

仅指代函数的名称不会产生冲突

```snap
tests/snap/semantic_tokens/conflict_ambiguity/03_using_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**注入类名（injected class name）**

注入类名用作构造函数名称时，仍保留类 Token

代码中的名称按类标注，其隐含的构造函数引用不产生额外高亮，`(` 仍不带 Token。

```snap
tests/snap/semantic_tokens/conflict_ambiguity/04_injected_class_name.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token 的正确性

clice 明确固定的 Token 标注行为，包括 clangd 曾处理错误的情况。

<!-- BEGIN GENERATED ITEMS: token_correctness -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**构造函数与析构函数**

构造函数和析构函数使用带有专用修饰符的方法 Token

析构函数名称标注为两个 Token：`~` 带有方法类型以及声明／定义修饰符，其后的类名仍是对类的引用。

```snap
tests/snap/semantic_tokens/token_correctness/01_constructors_destructors.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**匿名参数**

未命名参数不产生 Token

未命名参数类型后的标点仍不带 Token。

```snap
tests/snap/semantic_tokens/token_correctness/02_anonymous_parameters.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**运算符名称**

`operator` 关键字和调用处的标点保持原样

代码中的运算符名称由关键字和标点组成，因此不添加名称 Token：`operator` 保留其关键字分类，调用处的运算符符号不产生 Token。

```snap
tests/snap/semantic_tokens/token_correctness/03_operator_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板的析构函数**

`~` 的标注方式同样适用于模板

```snap
tests/snap/semantic_tokens/token_correctness/04_template_destructor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**转换运算符**

名称以关键字形式书写，转换调用不产生额外高亮

```snap
tests/snap/semantic_tokens/token_correctness/05_conversion_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板参数上的伪析构函数**

`~` 不产生高亮，类型名称保留其 Token 类型

```snap
tests/snap/semantic_tokens/token_correctness/06_pseudo_destructor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**显式默认与删除的成员**

特殊成员名称保留其定义 Token

```snap
tests/snap/semantic_tokens/token_correctness/07_defaulted_deleted.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 属性

<!-- BEGIN GENERATED ITEMS: attributes -->

<!-- BEGIN CAPABILITY: unsupported clangd#2209 -->

**属性名称**

属性名称及其表达式目前尚不获得语义 Token

```snap
tests/snap/semantic_tokens/attributes/01_attributes.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

宏定义体内的 Token 保留其词法类型；根据宏展开结果为它们添加高亮，将由未来的展开预览功能实现。

<!-- BEGIN GENERATED ITEMS: macros -->

<!-- BEGIN CAPABILITY: supported -->

**宏定义与展开**

宏定义和展开会获得语义 Token

```snap
tests/snap/semantic_tokens/macros/01_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开位置与实参**

展开处的名称标注为宏，代码中的实参保留其语义，定义体仍按词法标注

```snap
tests/snap/semantic_tokens/macros/02_macro_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2649 -->

**对象式宏与函数式宏**

对象式宏和函数式宏目前尚未使用不同的 Token 类型

```snap
tests/snap/semantic_tokens/macros/03_macro_kinds.cpp
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

## 非活动代码区域

未选中的预处理器分支内，每个 Token 都带有 `inactive` 修饰符，同时保留其词法类别，因此编辑器可以通过设置该修饰符的样式来淡化显示整个区域，同时保留原有的语法着色。非活动代码中没有分类的 Token（普通标识符和标点符号）以不带样式的 `identifier` 类型输出，使 Token 覆盖整个区域。clice 的 VS Code 扩展默认会淡化显示这些区域；其他编辑器则直接为该修饰符设置样式（例如 Neovim 中的 `@lsp.mod.inactive`）。

- [x] 淡化显示非活动预处理器分支（[clangd#132](https://github.com/clangd/clangd/issues/132)）
- [x] 正确处理 `#elif` 链中的非活动区域边界（[clangd#602](https://github.com/clangd/clangd/issues/602)）
- [x] 保留非活动区域内的语法高亮（[clangd#1664](https://github.com/clangd/clangd/issues/1664)）
- [x] 保持非活动区域与注释的区别（[clangd#1545](https://github.com/clangd/clangd/issues/1545)）
- [ ] 淡化显示不可达代码（[clangd#1828](https://github.com/clangd/clangd/issues/1828)）

## 格式字符串高亮

- [ ] `std::format` / `std::print` 占位符高亮（[clangd#1709](https://github.com/clangd/clangd/issues/1709)）
- [ ] 将无效的格式说明符高亮显示为错误

## 协议支持

- [x] 全文档语义 Token（`textDocument/semanticTokens/full`）
- [x] 以 UTF-16 码元为单位、采用差分编码的 Token 位置
- [ ] 指定范围的语义 Token（`textDocument/semanticTokens/range`）——仅计算可见视口内的 Token，这对大文件至关重要
- [ ] 增量更新（`textDocument/semanticTokens/full/delta`）——仅发送自上次响应以来的变化
