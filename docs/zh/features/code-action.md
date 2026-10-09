# 代码操作

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/code_action/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice 针对选区提供代码操作：依据编译器已掌握的信息生成或改写代码的重构、为头文件未声明的名字提供的快速修复，以及编译器和 clang-tidy 附在诊断上的修复。每个操作在列出时就已完整算好，应用时无需再等一次请求；编辑内容带有计算时所依据的文档版本，缓冲区一旦变动，编辑器就会拒绝它们。同一批操作也可以通过 `clice inspect code_action` 以无头方式运行。

操作锚定在选区覆盖的最内层构造上——方法声明、类名、一个 `switch`、一个 `auto`——因此列表始终很短：点击方法名，列出的是适用于该方法的操作；点击类名，列出的则是适用于该类的操作。

## 执行操作

在 VS Code 中，光标处的操作可以通过灯泡图标和 `Ctrl+.` 打开，重构还会列在编辑器右键菜单的 **Refactor...** 中（`Ctrl+Shift+R`）。重命名符号（`F2`）是一个单独的请求，不属于代码操作。

每种重构都有自己的代码操作种类（kind），因此编辑器可以只请求其中一种：

| 操作                                 | 种类                                             |
| ------------------------------------ | ------------------------------------------------ |
| 就地定义方法                         | `refactor.rewrite.define.inline`                 |
| 在类外定义函数，或定义到宿主源文件中 | `refactor.rewrite.define.outOfLine`              |
| 定义类中缺失的成员                   | `refactor.rewrite.define.missing`                |
| 实现纯虚方法                         | `refactor.rewrite.implementPureVirtuals`         |
| 生成逐成员构造函数                   | `refactor.rewrite.generateMemberwiseConstructor` |
| 为 switch 补上缺失的枚举分支         | `refactor.rewrite.populateSwitch`                |
| 展开推导出的类型                     | `refactor.rewrite.expandDeducedType`             |
| 按声明顺序重排定义                   | `refactor.rewrite.reorderDefinitions`            |
| 展开宏                               | `refactor.inline.macro`                          |
| 添加包含指令、应用诊断的修复         | `quickfix`                                       |

VS Code 扩展为每种重构提供一条命令，命令名是 `clice.` 加上它的种类，在命令面板中的标题取自操作本身（“Clice: Add Missing Enum Cases to Switch”）。只给出一个操作时，命令会直接应用它，否则让你从中选择。在 `keybindings.json` 中可以为它绑定按键：

```json
{
  "key": "ctrl+alt+s",
  "command": "clice.refactor.rewrite.populateSwitch",
  "when": "editorLangId == cpp"
}
```

一个种类也涵盖它下面的各个种类：指定 `refactor.rewrite.define` 时，VS Code 自带的 `editor.action.codeAction` 命令会列出定义光标处函数的所有方式。

```json
{
  "key": "ctrl+alt+d",
  "command": "editor.action.codeAction",
  "args": { "kind": "refactor.rewrite.define", "apply": "ifSingle" },
  "when": "editorLangId == cpp"
}
```

在 Neovim 中，`:LspCliceRefactor {kind}` 的作用相同。

## 定义函数

<!-- BEGIN GENERATED ITEMS: define -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**为已声明的方法生成定义**

方法声明处既可以就地补出函数体，也可以在类之后生成类外定义

类外定义会重复一遍声明，在名字前加上 `S::`，并去掉只属于声明的部分，例如默认实参。

```snap
tests/snap/code_action/define/01_method_out_of_line.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**只属于声明的说明符会被去掉**

`virtual`、`static`、`explicit`、`override` 和 `final` 不会出现在定义上

必须保留的说明符会留下，例如 `constexpr` 和 `noexcept`。

```snap
tests/snap/code_action/define/02_declaration_specifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按定义处作用域书写的返回类型**

指向嵌套类型或其他命名空间的返回类型，会写成在定义处能够解析的形式

参数类型和声明中一样在类作用域里查找，保持原样不变。

```snap
tests/snap/code_action/define/03_qualified_return_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板的成员**

类模板成员的定义会带上模板头，并在限定符中写出模板实参

模板头上不重复默认模板实参。

```snap
tests/snap/code_action/define/04_class_template_member.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**成员函数模板**

成员模板在类的模板头之后保留自己的模板头，但不带默认实参

带约束的模板会保留 requires 子句，定义必须重复这些子句。

```snap
tests/snap/code_action/define/05_member_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数、析构函数和运算符**

特殊成员函数按它们书写时所用的类名生成定义

转换函数和运算符保留完整的书写形式。

```snap
tests/snap/code_action/define/06_special_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**自由函数的声明**

已声明的自由函数会紧接声明之后、在同一个命名空间内生成定义

在命名空间内部，名字不需要限定符；返回类型按该作用域书写。

```snap
tests/snap/code_action/define/07_free_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**放在已有定义之后**

如果类在该文件中已有类外定义，新的定义会放在最后一个之后

限定符依照那个定义所在的作用域，而不是类的作用域。

```snap
tests/snap/code_action/define/08_placement_after_definitions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**一次定义所有缺失的成员**

在类名处，所有没有定义的成员函数会按声明顺序一次性生成定义

已有定义的成员、纯虚函数和显式默认（defaulted）的成员会被跳过。

```snap
tests/snap/code_action/define/09_missing_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**从定义处补出缺失的成员**

在类外定义内部，也可以为该类其余尚未定义的成员生成定义

源文件正是这样补全在别处声明的类：新生成的定义会接在已有定义之后。

```snap
tests/snap/code_action/define/10_missing_from_definition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**定义到宿主源文件中**

在头文件中，成员还可以定义到与该头文件一起编译的源文件里

定义使用完全限定名，并与该文件中该类的其他定义放在一起；已经在某个源文件中定义过的成员不会再次列出。模板、内联函数以及其他文件看不到的函数仍留在头文件中；其余函数在头文件里定义到类外时会标上 `inline`。

```snap
tests/snap/code_action/define/11_header_host/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套类的成员**

嵌套类的成员定义在最外层封闭类之后，限定符逐层写全

```snap
tests/snap/code_action/define/12_nested_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖返回类型**

返回类型用到类模板本身或其成员类型时，会通过模板形参写出限定

```snap
tests/snap/code_action/define/13_dependent_return_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**链接说明（linkage specification）中的声明**

以 C 链接声明的函数和其他函数一样可以生成定义，位置在链接块内部，或者单条声明形式之后

```snap
tests/snap/code_action/define/14_linkage_specification.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 实现接口

<!-- BEGIN GENERATED ITEMS: implement -->

<!-- BEGIN CAPABILITY: supported clangd#1037 -->

**实现纯虚方法**

从抽象基类派生的类，会为每个尚未实现的纯虚方法得到一条 `override` 声明

这些声明添加在类体末尾。

```snap
tests/snap/code_action/implement/01_pure_virtuals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**沿继承链的纯虚方法**

只为继承链上没有任何类实现过的方法生成声明，`class` 还会为它们加上 `public:` 标签

```snap
tests/snap/code_action/implement/02_inheritance_chain.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**面向派生类的类型**

在基类作用域中书写的参数类型和返回类型会加上限定，使其在派生类中能够解析

引用限定符和 const 属性会一并带过来。

```snap
tests/snap/code_action/implement/03_qualified_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**转换函数与指针参数**

转换函数没有返回类型要写出，参数的类型若把名字包在中间，也会保持这一形状

```snap
tests/snap/code_action/implement/04_conversion_and_pointers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**签名相同的多个基类**

一条声明同时重写所有基类中具有该签名的纯虚方法，其中任一个带 `noexcept` 时它也会带上

```snap
tests/snap/code_action/implement/05_shared_signatures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重写声明的说明符**

C 风格可变参数、`consteval`，以及基类方法是否为 `noexcept`，都会沿用到重写声明上

异常说明依赖于基类模板实参的方法不会得到声明。

```snap
tests/snap/code_action/implement/06_specifiers.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## switch 分支

<!-- BEGIN GENERATED ITEMS: switch_cases -->

<!-- BEGIN CAPABILITY: supported clangd#807 -->

**缺失的枚举分支**

对枚举做的 switch 会补上未处理的枚举项，每个后面跟一个 `break`

与已处理值相同的枚举项视为已覆盖。

```snap
tests/snap/code_action/switch_cases/01_missing_cases.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**插在 default 之前的分支**

存在 `default` 时，缺失的分支会紧挨着插在它前面并落入其中，从而保持原有行为

在 switch 内部的任意位置都会给出该操作。

```snap
tests/snap/code_action/switch_cases/02_before_default.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间中的无作用域枚举**

当 switch 位于枚举所在命名空间之外时，无作用域枚举的枚举项会带上该命名空间的限定

```snap
tests/snap/code_action/switch_cases/03_unscoped_enum.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**完整的 switch 不给出操作**

处理了全部枚举项的 switch，或者对非枚举值做的 switch，不会给出任何操作

```snap
tests/snap/code_action/switch_cases/04_complete_switch.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**覆盖整个 switch 的选区**

选区覆盖整条语句时，给出的操作与光标停在语句内部时相同

```snap
tests/snap/code_action/switch_cases/05_selection_range.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖模板的标签**

switch 中有标签依赖模板参数时不给出操作，因为只有实例化之后才知道它覆盖了哪些枚举项

模板中的 switch 若标签不依赖模板参数，仍会像在其他地方一样补全分支。

```snap
tests/snap/code_action/switch_cases/06_dependent_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明变量的分支**

没有 `default` 时，若 switch 在自身作用域内声明了变量，缺失的分支会插在它的第一个标签之前，因为放在声明之后的标签会跳过该声明

原有的分支都不会落入插在那里的分支。

```snap
tests/snap/code_action/switch_cases/07_declaring_section.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

<!-- BEGIN GENERATED ITEMS: deduced_type -->

<!-- BEGIN CAPABILITY: supported -->

**展开声明中的 auto**

变量声明中的 `auto` 会替换为推导出的类型，限定符和声明符保持原位

```snap
tests/snap/code_action/deduced_type/01_auto_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导出的返回类型**

函数推导出的 `auto` 返回类型会展开为推导结果，并按函数所在作用域书写

```snap
tests/snap/code_action/deduced_type/02_auto_return.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开 decltype**

`decltype` 说明符会展开为它所表示的类型；如果该类型本身也是用 `decltype` 声明的，会沿着它继续展开

```snap
tests/snap/code_action/deduced_type/03_decltype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无法命名的类型保持 auto**

Lambda、依赖类型、结构化绑定（structured bindings）以及在该声明中无法命名的类型不会展开

以下类型无法命名：另一个函数内部的局部类型、声明无权访问的成员类型，以及标准名字尚未声明时 `sizeof` 的类型（MSVC 兼容模式会隐式声明 `size_t`）。

```snap
tests/snap/code_action/deduced_type/04_unnameable_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**转发引用（forwarding reference）与声明符类型**

`auto&&` 绑定到左值时，推导出的引用会同时替换掉这两个 Token，而把名字包在中间的类型保持不动

```snap
tests/snap/code_action/deduced_type/05_forwarding_reference.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按作用域书写的名字**

名字省去外层命名空间的程度，以更短的名字仍能找到同一类型为限

被离展开处更近的声明隐藏的名字会保留限定符；连完全限定名也被隐藏时，则从全局作用域写起。

```snap
tests/snap/code_action/deduced_type/06_shadowed_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**内置类型的标准名字**

在标准名字已声明的位置，`sizeof`、指针相减和 `nullptr` 的类型会展开为对应的标准名字

看不到 `std::nullptr_t` 的声明时，`nullptr` 的类型写作 `decltype(nullptr)`。

```snap
tests/snap/code_action/deduced_type/07_standard_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导出的常量指针**

`const` 写在推导出指针的 `auto` 前面时，会移到 `*` 之后，使指针本身保持为常量

`const` 与 `auto` 之间还有其他说明符时，声明保持原样。

```snap
tests/snap/code_action/deduced_type/08_const_pointer.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

<!-- BEGIN GENERATED ITEMS: macro -->

<!-- BEGIN CAPABILITY: supported clangd#820 -->

**展开宏调用**

宏调用会替换为它展开后的 Token

实参会被代入；在宏名处或其实参内部的任意位置都会给出该操作。

```snap
tests/snap/code_action/macro/01_expand_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套的宏完全展开**

宏体中调用了其他宏时，会一直展开到最终的 Token

```snap
tests/snap/code_action/macro/02_nested_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预处理指令中的宏引用与空宏**

预处理条件中出现的宏名，无论位于条件的哪一行，都不是可以替换的展开，而展开为空的宏会被删除

```snap
tests/snap/code_action/macro/03_directives_and_empty.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**执行 pragma 的宏**

展开时会执行 `_Pragma` 运算符的宏不给出展开操作，因为 pragma 不会留下可以写回原处的 Token

```snap
tests/snap/code_action/macro/04_pragma_operator.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Token 不会粘连**

展开结果会插入适当的空格，使其中的 Token 既不会彼此粘连，也不会与紧贴宏调用书写的文本粘连

```snap
tests/snap/code_action/macro/05_token_boundaries.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 缺失的包含指令

<!-- BEGIN GENERATED ITEMS: include -->

<!-- BEGIN CAPABILITY: supported clangd#1017 -->

**标准库的包含指令**

无法解析的标准库名字会依据标准库映射，给出声明它的头文件

指令插入在文件开头那组包含指令之后。非限定的名字还会尝试 `std` 命名空间。

```snap
tests/snap/code_action/include/01_standard_library.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**项目符号的包含指令**

名字声明在某个项目头文件中，而当前文件没有包含它时，会给出该头文件，路径按相对当前文件的形式书写

候选来自项目索引：`lib.h` 之所以已知，是因为另一个源文件包含了它。

```snap
tests/snap/code_action/include/02_project_header/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文件的第一条包含指令**

没有任何包含指令的文件会在开头插入该指令；若存在 `#pragma once`，则插在其后

```snap
tests/snap/code_action/include/03_no_include_yet.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**条件编译下的包含指令**

嵌套在条件编译里的包含指令不是必须始终生效的指令该去的位置：新指令跟在文件自身层级的最后一条之后，或者跟在头文件保护（include guard）之后

```snap
tests/snap/code_action/include/04_conditional_includes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌入的与尾随的包含指令**

位于 `extern "C"` 或类型体内部的包含指令，以及跟在代码之后的包含指令，都不是新指令该去的位置：新指令会加入文件开头的那组包含指令

```snap
tests/snap/code_action/include/05_trailing_includes.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 诊断修复

选区下的诊断会把它的修复作为快速修复提供，编译器和 clang-tidy 的诊断都是如此。需要改动宏定义内部或其他文件的修复不会提供。

<!-- BEGIN GENERATED ITEMS: fix -->

<!-- BEGIN CAPABILITY: supported -->

**编译器修复**

编译器附在诊断上的修复会作为快速修复提供

只有一处编辑时，标题直接写出这处编辑。

```snap
tests/snap/code_action/fix/01_compiler_fix.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy 修复**

clang-tidy 检查结果的修复会作为快速修复提供

文件所在目录及其上级目录中没有 `.clang-tidy` 时，运行的是一小组默认检查。

```snap
tests/snap/code_action/fix/02_tidy_fix.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**来自 note 的修复**

每条带有修复的 note 都会把它作为快速修复提供，标题取自该 note

```snap
tests/snap/code_action/fix/03_note_fixes.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 重排定义

<!-- BEGIN GENERATED ITEMS: reorder -->

<!-- BEGIN CAPABILITY: supported -->

**按声明顺序重排定义**

类成员的类外定义会重新排列，以与类中的声明顺序一致

每个定义会连同紧挨其上的注释块一起移动。

```snap
tests/snap/code_action/reorder/01_class_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间块各自重排**

写在不同命名空间块中的定义只在各自块内重排，绝不跨块

```snap
tests/snap/code_action/reorder/02_namespace_blocks.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**自由函数按声明顺序排列**

在自由函数的定义处，与它一同声明的那些函数的定义会按声明顺序重排

```snap
tests/snap/code_action/reorder/03_free_functions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**行尾注释留在原处**

位于某个定义行尾的注释会跟着该定义移动，不会跟着下面那个定义走

```snap
tests/snap/code_action/reorder/04_trailing_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**用到中间内容的定义**

如果移动某个定义会把它挪到它所用的东西之前，例如夹在这些定义之间的变量或宏定义，该定义就留在原处，其余定义围绕它重排

```snap
tests/snap/code_action/reorder/05_dependencies.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 构造函数

<!-- BEGIN GENERATED ITEMS: constructor -->

<!-- BEGIN CAPABILITY: supported -->

**逐成员构造函数**

为类生成按顺序接收全部字段的构造函数，标量按值传递，可复制的类按 const 引用传递

```snap
tests/snap/code_action/constructor/01_memberwise.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不重复生成已有的构造函数**

当类已经声明了参数个数与字段数相同的构造函数时，不再生成构造函数

```snap
tests/snap/code_action/constructor/02_existing_constructor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单字段构造函数带 explicit**

只有一个字段的类会得到 `explicit` 构造函数；若类以其他访问段结尾，构造函数会放在 `public:` 标签之下

```snap
tests/snap/code_action/constructor/03_single_field.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**声明符形状的字段类型**

字段的类型若把名字包在中间，例如函数指针，在参数中会保持这一形状

```snap
tests/snap/code_action/constructor/04_declarator_fields.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有默认构造函数的基类**

当基类需要自己的初始化项时，不会生成构造函数，因为逐成员构造函数只初始化字段

```snap
tests/snap/code_action/constructor/05_base_without_default.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**默认构造函数被删除的基类**

当基类的默认构造函数被删除时，无论是显式删除，还是因引用成员或没有初始化的 const 成员而隐式删除，同样不会生成逐成员构造函数

若基类自己初始化了全部字段，其中的 const 成员不会让它失去默认构造函数。

```snap
tests/snap/code_action/constructor/06_implicitly_deleted_base.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**只能移动的字段**

类型只能移动、不能复制的字段按值接收并移动到字段中，右值引用字段则经 `std::move` 绑定到传入的实参

类之前没有任何地方声明 `std::move` 时，会为文件加上 `#include <utility>`。既不能复制也不能移动的类不会得到构造函数。

```snap
tests/snap/code_action/constructor/07_move_only_fields.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 格式化

当文件适用项目的 clang-format 样式时，生成的文本会按该样式格式化，于是一段定义或一组 `case` 标签会融入周围代码的排版。没有样式，或者样式为 `DisableFormat` 时，文本保持上面示例中的排版。

## 已知限制

- 定义到宿主源文件时，位置在索引已知的、该文件中该类成员的最后一个定义之后；文件中一个都没有时放在文件末尾。哪个源文件充当头文件的宿主，取决于该头文件的编译上下文。
- 只有当项目索引知道某个定义时，已在另一个源文件中定义的成员才会被“定义缺失的成员”操作排除；关闭索引后，每个未定义的成员都会列出。
- 返回类型用到依赖基类（dependent base class）的成员时，会原样照搬到类外定义中，在那里可能需要 `typename` 和基类的限定符。
- 缺失包含指令的候选来自标准库映射，以及项目索引见过的头文件；没有任何已索引源文件包含过的头文件不会被建议。

## 尚未实现

提取函数与变量、内联函数与变量、在头文件与源文件之间移动定义、把无作用域枚举转换为有作用域枚举，以及在所有调用处修改函数签名。
