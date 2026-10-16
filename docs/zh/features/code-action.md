# 代码操作

<!-- The cards in the sections below are generated from the documented cases
     in tests/code_actions/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice 针对选区提供代码操作：依据编译器已掌握的信息生成或改写代码的重构、为头文件未声明的名字提供的快速修复，以及编译器和 clang-tidy 附在诊断上的修复。每个操作在列出时就已完整算好，应用时无需再等一次请求；编辑内容带有计算时所依据的文档版本，缓冲区一旦变动，编辑器就会拒绝它们。同一批操作也可以通过 `clice inspect code-action` 以无头方式运行。

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

已声明的函数可以就地定义，也可以在所属类之后定义，或定义在与头文件一同编译的源文件中。在类名处执行操作，会一次性为所有尚未定义的成员生成定义。

<!-- BEGIN GENERATED ITEMS: code_actions/define -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**为已声明的方法生成定义**

方法声明处既可以就地补出函数体，也可以在类之后生成类外定义

类外定义会沿用声明的写法，在名称前加上 `S::`，并去掉默认实参等仅属于声明的内容。

```case
tests/code_actions/define/01_method_out_of_line
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板的成员**

类模板成员的定义会带上模板头，并在限定符中写出模板实参

模板头上不重复默认模板实参。

```case
tests/code_actions/define/04_class_template_member
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数、析构函数和运算符**

特殊成员函数按它们书写时所用的类名生成定义

转换函数和运算符保留完整的书写形式。

```case
tests/code_actions/define/06_special_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**自由函数的声明**

已声明的自由函数会紧接声明之后、在同一个命名空间内生成定义

在命名空间内，函数名无需限定符；返回类型采用适用于该作用域的写法。

```case
tests/code_actions/define/07_free_function
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**放在已有定义之后**

如果类在该文件中已有类外定义，新的定义会放在最后一个之后

限定符依照那个定义所在的作用域，而不是类的作用域。

```case
tests/code_actions/define/08_placement_after_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**一次定义所有缺失的成员**

在类名处，所有没有定义的成员函数会按声明顺序一次性生成定义

已有定义的成员、纯虚方法和已默认化（defaulted）的成员保持不变。

```case
tests/code_actions/define/09_missing_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**从定义处补出缺失的成员**

在类外定义内部，也可以为该类其余尚未定义的成员生成定义

源文件通过这种方式补全在其他地方声明的类：新定义会与文件中已有的定义放在一起。

```case
tests/code_actions/define/10_missing_from_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**定义到宿主源文件中**

在头文件中，成员还可以定义到与该头文件一起编译的源文件里

生成的定义使用完全限定名，并与该类在该文件中的其他定义放在一起；对于已在某个源文件中定义的成员，不再提供此操作。模板、内联函数和对其他文件不可见的函数仍在头文件中定义；其余在头文件中定义于类外的函数会标记为 `inline`。

```case
tests/code_actions/define/11_host_source
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 实现接口

对于派生自抽象基类的类，会为每个仍需重写的纯虚方法生成声明。

<!-- BEGIN GENERATED ITEMS: code_actions/implement -->

<!-- BEGIN CAPABILITY: supported clangd#1037 -->

**实现纯虚方法**

从抽象基类派生的类，会为每个尚未实现的纯虚方法得到一条 `override` 声明

这些声明添加在类体末尾。

```case
tests/code_actions/implement/01_pure_virtuals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**沿继承链的纯虚方法**

只为继承链上没有任何类实现过的方法生成声明，`class` 还会为它们加上 `public:` 标签

```case
tests/code_actions/implement/02_inheritance_chain
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 构造函数

为类生成一个构造函数，用对应的参数初始化每个字段。

<!-- BEGIN GENERATED ITEMS: code_actions/constructor -->

<!-- BEGIN CAPABILITY: supported -->

**逐成员构造函数**

为类生成按顺序接收全部字段的构造函数，标量按值传递，可复制的类按 const 引用传递

```case
tests/code_actions/constructor/01_memberwise
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不重复生成已有的构造函数**

当类已经声明了参数个数与字段数相同的构造函数时，不再生成构造函数

```case
tests/code_actions/constructor/02_existing_constructor
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单字段构造函数带 explicit**

只有一个字段的类会得到 `explicit` 构造函数；若类以其他访问段结尾，构造函数会放在 `public:` 标签之下

```case
tests/code_actions/constructor/03_single_field
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有默认构造函数的基类**

当基类需要自己的初始化项时，不会生成构造函数，因为逐成员构造函数只初始化字段

```case
tests/code_actions/constructor/05_base_without_default
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**只能移动的字段**

类型只能移动、不能复制的字段按值接收并移动到字段中，右值引用字段则经 `std::move` 绑定到传入的实参

如果类之前没有任何 `std::move` 声明，就会在文件中添加 `#include <utility>`。既不能复制也不能移动的类不会生成构造函数。

```case
tests/code_actions/constructor/07_move_only_fields
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## switch 分支

针对枚举的 `switch` 会为每个尚未处理的枚举项添加一个 `case` 分支。

<!-- BEGIN GENERATED ITEMS: code_actions/switch_cases -->

<!-- BEGIN CAPABILITY: supported clangd#807 -->

**缺失的枚举分支**

对枚举做的 switch 会补上未处理的枚举项，每个后面跟一个 `break`

与已处理值相同的枚举项视为已覆盖。

```case
tests/code_actions/switch_cases/01_missing_cases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**插在 default 之前的分支**

存在 `default` 时，缺失的分支会紧挨着插在它前面并落入其中，从而保持原有行为

在 switch 内部的任意位置都会给出该操作。

```case
tests/code_actions/switch_cases/02_before_default
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间中的无作用域枚举**

当 switch 位于枚举所在命名空间之外时，无作用域枚举的枚举项会带上该命名空间的限定

```case
tests/code_actions/switch_cases/03_unscoped_enum
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**完整的 switch 不给出操作**

处理了全部枚举项的 switch，或者对非枚举值做的 switch，不会给出任何操作

```case
tests/code_actions/switch_cases/04_complete_switch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

将 `auto` 或 `decltype` 替换为其所代表的类型，并采用能在当前位置正确指代该类型的写法。

<!-- BEGIN GENERATED ITEMS: code_actions/deduced_type -->

<!-- BEGIN CAPABILITY: supported -->

**显式写出 auto 推导的类型**

在 auto 声明处，可以将 auto 替换为推导出的类型

`const`、`&` 和 `*` 等限定符和声明符保持原位。

```case
tests/code_actions/deduced_type/01_auto_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导出的返回类型**

函数推导出的 `auto` 返回类型会展开为推导结果，并按函数所在作用域书写

```case
tests/code_actions/deduced_type/02_auto_return
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开 decltype**

`decltype` 说明符会展开为它所表示的类型；若该类型本身也是用 `decltype` 声明的，则继续展开

```case
tests/code_actions/deduced_type/03_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无法命名的类型保持 auto**

Lambda、依赖类型、结构化绑定（structured bindings）以及在该声明中无法命名的类型不会展开

以下类型无法命名：另一个函数内部的局部类型、声明无权访问的成员类型，以及标准名字尚未声明时 `sizeof` 的类型（MSVC 兼容模式会隐式声明 `size_t`）。

```case
tests/code_actions/deduced_type/04_unnameable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按作用域书写的名字**

名字省去外层命名空间的程度，以更短的名字仍能找到同一类型为限

被离展开处更近的声明隐藏的名字会保留限定符；连完全限定名也被隐藏时，则从全局作用域写起。

```case
tests/code_actions/deduced_type/06_shadowed_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**内置类型的标准名字**

在标准名字已声明的位置，`sizeof`、指针相减和 `nullptr` 的类型会展开为对应的标准名字

这里由头文件声明这些名字，就像标准头文件那样。

```case
tests/code_actions/deduced_type/07_standard_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 重排定义

与声明分开编写的定义会按声明顺序重排。

<!-- BEGIN GENERATED ITEMS: code_actions/reorder -->

<!-- BEGIN CAPABILITY: supported -->

**按声明顺序重排定义**

类成员的类外定义会重新排列，以与类中的声明顺序一致

每个定义会连同紧挨其上的注释块一起移动。

```case
tests/code_actions/reorder/01_class_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**自由函数按声明顺序排列**

在自由函数的定义处，与它一同声明的那些函数的定义会按声明顺序重排

```case
tests/code_actions/reorder/03_free_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**用到中间内容的定义**

如果移动某个定义会把它挪到它所用的东西之前，例如夹在这些定义之间的变量或宏定义，该定义就留在原处，其余定义围绕它重排

```case
tests/code_actions/reorder/05_dependencies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

宏调用会替换为它展开后的代码。

<!-- BEGIN GENERATED ITEMS: code_actions/macro -->

<!-- BEGIN CAPABILITY: supported clangd#820 -->

**展开宏调用**

宏调用会替换为它展开后的 Token

展开时会代入实参；在宏名或实参内的任意位置都可使用此操作。

```case
tests/code_actions/macro/01_expand_macro
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套的宏完全展开**

宏体中调用了其他宏时，会一直展开到最终的 Token

```case
tests/code_actions/macro/02_nested_expansion
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 缺失的包含指令

若已包含的头文件均未声明某个名字，则会建议包含标准库或项目中声明了该名字的头文件。

<!-- BEGIN GENERATED ITEMS: code_actions/include -->

<!-- BEGIN CAPABILITY: supported clangd#1017 -->

**标准库的包含指令**

无法解析的标准库名字会依据标准库映射，给出声明它的头文件

该指令会插在文件顶部已有的包含指令之后。

```case
tests/code_actions/include/01_standard_library
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**项目符号的包含指令**

名字声明在某个项目头文件中，而当前文件没有包含它时，会给出该头文件，路径按相对当前文件的形式书写

候选头文件来自项目索引：`lib.h` 已被另一个源文件包含，因此能在索引中找到。

```case
tests/code_actions/include/02_project_header
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文件的第一条包含指令**

没有任何包含指令的文件会在开头插入该指令；若存在 `#pragma once`，则插在其后

```case
tests/code_actions/include/03_no_include_yet
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**条件编译下的包含指令**

嵌套在条件编译里的包含指令不是必须始终生效的指令该去的位置：新指令跟在文件自身层级的最后一条之后，或者跟在头文件保护（include guard）之后

```case
tests/code_actions/include/04_conditional_includes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 诊断修复

选区下的诊断会把它的修复作为快速修复提供，编译器和 clang-tidy 的诊断都是如此。需要改动宏定义内部或其他文件的修复不会提供。

<!-- BEGIN GENERATED ITEMS: code_actions/fix -->

<!-- BEGIN CAPABILITY: supported -->

**编译器修复**

编译器附在诊断上的修复会作为快速修复提供

只有一处编辑时，标题直接写出这处编辑。

```case
tests/code_actions/fix/01_compiler_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy 修复**

clang-tidy 检查结果的修复会作为快速修复提供

文件所在目录及其上级目录中没有 `.clang-tidy` 时，运行的是一小组默认检查。

```case
tests/code_actions/fix/02_tidy_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**来自 note 的修复**

每条带有修复的 note 都会把它作为快速修复提供，标题取自该 note

```case
tests/code_actions/fix/03_note_fixes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 格式化

如果项目中有适用于该文件的 clang-format 样式，生成的文本就会按该样式格式化，使定义或一组 `case` 标签的排版与周围代码一致。

<!-- BEGIN GENERATED ITEMS: code_actions/formatting -->

<!-- BEGIN CAPABILITY: supported -->

**项目格式化样式**

生成的代码遵循适用于该文件的 clang-format 样式

如果文件所在目录及其上级目录中没有 `.clang-format`，或设置了 `DisableFormat`，文本就会保留其他示例中所示的排版。

```case
tests/code_actions/formatting/01_project_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 已知限制

- 定义到宿主源文件时，位置在索引已知的、该文件中该类成员的最后一个定义之后；文件中一个都没有时放在文件末尾。哪个源文件充当头文件的宿主，取决于该头文件的编译上下文。
- 只有当项目索引知道某个定义时，已在另一个源文件中定义的成员才会被“定义缺失的成员”操作排除；关闭索引后，每个未定义的成员都会列出。
- 返回类型用到依赖基类（dependent base class）的成员时，会原样照搬到类外定义中，在那里可能需要 `typename` 和基类的限定符。
- 缺失包含指令的候选来自标准库映射，以及项目索引见过的头文件；没有任何已索引源文件包含过的头文件不会被建议。

## 尚未实现

提取函数与变量、内联函数与变量、在头文件与源文件之间移动定义、把无作用域枚举转换为有作用域枚举，以及在所有调用处修改函数签名。
