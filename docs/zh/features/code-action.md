# 代码操作

<!-- The cards in the sections below are generated from the documented cases
     in tests/code_actions/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

clice 针对选区提供代码操作：依据编译器掌握的信息生成或改写代码的重构、为头文件尚未声明的名称提供的快速修复，以及编译器和 clang-tidy 附在诊断上的修复。每项操作在给出时就已计算完毕，应用时无需再等待第二次请求；编辑内容附带计算时所依据的文档版本，一旦缓冲区已继续修改，编辑器就会拒绝应用。同一批操作也可通过 `clice inspect code-action` 以无头方式运行。

每项操作都锚定在选区覆盖的最内层结构上——方法声明、类名、`switch`、`auto`——从而保持列表简短：点击方法名给出适用于该方法的操作，点击类名给出适用于该类的操作。

## 执行操作

在 VS Code 中，光标处的操作可通过灯泡图标或 `Ctrl+.` 打开，重构还会列在编辑器右键菜单的 **Refactor...** 中（`Ctrl+Shift+R`）。重命名符号（`F2`）是独立的请求，不属于代码操作。

每种重构都有各自的代码操作种类（kind），编辑器可以只请求其中一种：

| 操作                                 | 种类                                             |
| ------------------------------------ | ------------------------------------------------ |
| 就地定义方法                         | `refactor.rewrite.define.inline`                 |
| 在类外定义函数，或定义到宿主源文件中 | `refactor.rewrite.define.outOfLine`              |
| 定义类中缺失的成员                   | `refactor.rewrite.define.missing`                |
| 实现纯虚方法                         | `refactor.rewrite.implementPureVirtuals`         |
| 生成逐成员构造函数                   | `refactor.rewrite.generateMemberwiseConstructor` |
| 为 switch 补全缺失的枚举分支         | `refactor.rewrite.populateSwitch`                |
| 展开推导类型                         | `refactor.rewrite.expandDeducedType`             |
| 按声明顺序重排定义                   | `refactor.rewrite.reorderDefinitions`            |
| 展开宏                               | `refactor.inline.macro`                          |
| 添加头文件包含、应用诊断修复         | `quickfix`                                       |

VS Code 扩展为每种重构提供了一条命令，命令名为 `clice.` 加上对应种类，在命令面板中的标题取自该操作（“clice: Add Missing Enum Cases to Switch”）。若只给出一个候选操作则立即应用，否则让你选择。可在 `keybindings.json` 中为其绑定快捷键：

```json
{
  "key": "ctrl+alt+s",
  "command": "clice.refactor.rewrite.populateSwitch",
  "when": "editorLangId == cpp"
}
```

一个种类也涵盖其下属子种类：传入 `refactor.rewrite.define` 时，VS Code 自带的 `editor.action.codeAction` 命令会列出定义光标处函数的所有方式。

```json
{
  "key": "ctrl+alt+d",
  "command": "editor.action.codeAction",
  "args": { "kind": "refactor.rewrite.define", "apply": "ifSingle" },
  "when": "editorLangId == cpp"
}
```

在 Neovim 中，`:LspCliceRefactor {kind}` 的效果相同。

## 定义函数

已声明的函数可以就地定义、在所属类之后定义，或定义到与头文件一同编译的源文件中。在类名上执行操作时，所有尚未定义的成员会一次性生成定义。

<!-- BEGIN GENERATED ITEMS: code_actions/define -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**定义已声明的方法**

方法声明处既可就地补出函数体，也可在所属类之后生成类外定义

类外定义复写声明并在函数名前加上 `S::`，同时去掉默认实参等仅属于声明的部分。

```case
tests/code_actions/define/01_method_out_of_line
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**类模板成员**

类模板成员的定义带有模板头，并在限定符中写出模板实参

模板头中不会重复默认模板实参。

```case
tests/code_actions/define/04_class_template_member
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**构造函数、析构函数与运算符**

特殊成员函数按其书写时使用的类名生成定义

转换函数和运算符保留完整写法。

```case
tests/code_actions/define/06_special_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**自由函数声明**

已声明的自由函数紧接声明之后、在同一命名空间内生成定义

命名空间内的函数名无需限定符；返回类型采用适用于该作用域的写法。

```case
tests/code_actions/define/07_free_function
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**放在已有定义之后**

若该类在当前文件中已有类外定义，新定义会放在最后一个之后

限定符遵循那个定义所在的作用域，而非类声明所在的作用域。

```case
tests/code_actions/define/08_placement_after_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**定义所有缺失的成员**

在类名处，所有尚无定义的成员函数会按声明顺序一次性生成定义

已有定义的成员、纯虚方法以及默认化（defaulted）成员保持不变。

```case
tests/code_actions/define/09_missing_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**从定义处补全缺失成员**

在类外定义内部，同样可以为该类其余未定义的成员生成定义

源文件正是这样补全在他处声明的类的：新定义会并入文件中已有的定义之后。

```case
tests/code_actions/define/10_missing_from_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**定义到宿主源文件中**

在头文件中，成员还可以定义到与该头文件一同编译的源文件里

生成的定义带完整限定符，并与该类在目标文件中的其他定义放在一起；已在某个源文件中定义过的成员不再提供此操作。模板、内联函数以及对其他文件不可见的函数仍留在头文件中；其余在头文件中于类外定义的函数都会标上 `inline`。

```case
tests/code_actions/define/11_host_source
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 实现接口

继承自抽象基类的派生类会为每个仍需重写的纯虚方法获得一条声明。

<!-- BEGIN GENERATED ITEMS: code_actions/implement -->

<!-- BEGIN CAPABILITY: supported clangd#1037 -->

**实现纯虚方法**

继承自抽象基类的派生类会为每个尚未实现的纯虚方法补上一条 `override` 声明

这些声明插入在类体末尾。

```case
tests/code_actions/implement/01_pure_virtuals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**继承链上的纯虚方法**

只声明整条继承链上没有任何类实现过的方法，且 `class` 会为它们加上 `public:` 标签

```case
tests/code_actions/implement/02_inheritance_chain
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 构造函数

为类生成构造函数，用对应参数初始化每个字段。

<!-- BEGIN GENERATED ITEMS: code_actions/constructor -->

<!-- BEGIN CAPABILITY: supported -->

**逐成员构造函数**

为类生成按顺序接收每个字段的构造函数，标量按值传递，可拷贝的类按 const 引用传递

```case
tests/code_actions/constructor/01_memberwise
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不重复生成已有构造函数**

若类已声明了参数个数与字段数相同的构造函数，则不再生成

```case
tests/code_actions/constructor/02_existing_constructor
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**单字段构造函数带 explicit**

只有一个字段的类会生成 `explicit` 构造函数；若类末尾处于其他访问控制段，则放在 `public:` 标签之下

```case
tests/code_actions/constructor/03_single_field
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**基类无默认构造函数**

若基类需要显式初始化，则不会生成构造函数，因为逐成员构造函数只初始化字段

```case
tests/code_actions/constructor/05_base_without_default
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**仅可移动的字段**

类型可移动但不可拷贝的字段按值接收并移动构造，右值引用字段则经 `std::move` 绑定传入的实参

文件会补上 `#include <utility>`，只要类之前尚未声明 `std::move`。既不可拷贝也不可移动的类不会生成构造函数。

```case
tests/code_actions/constructor/07_move_only_fields
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## switch 分支

针对枚举的 `switch` 会为每个尚未处理的枚举项补上一条 `case` 分支。

<!-- BEGIN GENERATED ITEMS: code_actions/switch_cases -->

<!-- BEGIN CAPABILITY: supported clangd#807 -->

**缺失的枚举分支**

针对枚举的 switch 会补上尚未处理的枚举项，每项后接一条 `break`

与已处理值相等的枚举项视为已覆盖。

```case
tests/code_actions/switch_cases/01_missing_cases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**插在 default 之前的分支**

存在 `default` 时，缺失的分支会紧插在它前面并直落（fall through）进入其中，保持原有行为

在 switch 内部的任意位置都会提供该操作。

```case
tests/code_actions/switch_cases/02_before_default
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**命名空间中的无作用域枚举**

若 switch 位于枚举所属命名空间之外，无作用域枚举的枚举项会带上该命名空间限定

```case
tests/code_actions/switch_cases/03_unscoped_enum
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**完整的 switch 不提供操作**

已处理全部枚举项的 switch，或针对非枚举值的 switch，不提供任何操作

```case
tests/code_actions/switch_cases/04_complete_switch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 推导类型

将 `auto` 或 `decltype` 替换为其所代表的类型，并采用在书写位置能够指代该类型的写法。

<!-- BEGIN GENERATED ITEMS: code_actions/deduced_type -->

<!-- BEGIN CAPABILITY: supported -->

**展开 auto 类型**

auto 声明处可将 auto 替换为推导出的类型

`const`、`&` 和 `*` 等限定符与声明符保留在原位。

```case
tests/code_actions/deduced_type/01_auto_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**推导的返回类型**

函数由 `auto` 推导出的返回类型会展开为具体类型，采用适用于该函数作用域的写法

```case
tests/code_actions/deduced_type/02_auto_return
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**展开 decltype**

`decltype` 说明符会展开为它所表示的类型，若该类型本身也是用 `decltype` 声明的则继续穿透展开

```case
tests/code_actions/deduced_type/03_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无法命名的类型保持 auto**

Lambda、依赖类型、结构化绑定（structured bindings）以及在声明处无法命名的类型不会展开

属于另一个函数局部的类型、当前声明无权访问的成员类型，以及尚未声明标准名称时 `sizeof` 的结果类型都无法命名（MSVC 兼容模式会隐式声明 `size_t`）。

```case
tests/code_actions/deduced_type/04_unnameable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按作用域书写的名称**

名称省去外层命名空间时，以缩短后的名称仍能查找到同一类型为限

被离展开处更近的声明遮蔽的名称会保留限定符；若连带限定符的名称也被遮蔽，则从全局作用域写起。

```case
tests/code_actions/deduced_type/06_shadowed_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**内置类型的标准名称**

`sizeof`、指针差值和 `nullptr` 的类型在对应标准名称已声明的位置会展开为标准名称

本例由头文件声明这些名称，与标准头文件的做法一致。

```case
tests/code_actions/deduced_type/07_standard_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 重排定义

与声明分开的定义会按声明顺序重排。

<!-- BEGIN GENERATED ITEMS: code_actions/reorder -->

<!-- BEGIN CAPABILITY: supported -->

**按声明顺序重排定义**

类成员的类外定义会重排，以匹配类中的声明顺序

每个定义都会连同紧邻其上的注释块一起移动。

```case
tests/code_actions/reorder/01_class_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按声明顺序重排自由函数**

在自由函数定义处，与它并列声明的各函数定义会按声明顺序重排

```case
tests/code_actions/reorder/03_free_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**依赖中间内容的定义**

若移动某个定义会使其排在它所使用的内容（如夹在各定义之间的变量或宏定义）之前，该定义会留在原处，其余定义围绕它重排

```case
tests/code_actions/reorder/05_dependencies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 宏

将宏调用替换为它展开后的代码。

<!-- BEGIN GENERATED ITEMS: code_actions/macro -->

<!-- BEGIN CAPABILITY: supported -->

**展开宏调用**

宏调用会替换为它展开后的 Token

实参会完成代入；光标位于宏名或其实参内的任意位置时都会提供该操作。

```case
tests/code_actions/macro/01_expand_macro
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套宏完全展开**

宏体中调用了其他宏时，会一直展开为最终的 Token

```case
tests/code_actions/macro/02_nested_expansion
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 缺失的头文件包含

若已包含的头文件均未声明某个名称，则会给出标准库或项目中声明该名称的头文件。

<!-- BEGIN GENERATED ITEMS: code_actions/include -->

<!-- BEGIN CAPABILITY: supported -->

**标准库头文件包含**

未解析的标准库名称会根据标准库映射给出声明它的头文件

包含指令会插入在文件顶部已有的包含指令之后。

```case
tests/code_actions/include/01_standard_library
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**项目符号的头文件包含**

名称声明在当前文件尚未包含的项目头文件中时，会给出该头文件，路径按相对于当前文件的形式书写

候选头文件来自项目索引：因为另一个源文件包含了 `lib.h`，索引已知该头文件。

```case
tests/code_actions/include/02_project_header
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文件的首条包含指令**

尚无包含指令的文件会在文件开头插入该指令；若有 `#pragma once` 则插在其后

```case
tests/code_actions/include/03_no_include_yet
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**条件编译下的包含指令**

嵌套在特性条件编译块内的包含指令不适合接纳必须始终生效的新指令：新指令会跟在文件顶层最后一条包含指令之后，或跟在头文件保护宏（include guard）之后

```case
tests/code_actions/include/04_conditional_includes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 诊断修复

选区下的诊断会将其附带的修复作为快速修复提供，编译器和 clang-tidy 的诊断均如此。需要修改宏定义内部或其他文件的修复不会提供。

<!-- BEGIN GENERATED ITEMS: code_actions/fix -->

<!-- BEGIN CAPABILITY: supported -->

**编译器修复**

编译器附在诊断上的修复会作为快速修复提供

只有一处修改时，标题会直接写出修改内容。

```case
tests/code_actions/fix/01_compiler_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy 修复**

clang-tidy 检查结果的修复会作为快速修复提供

文件上层目录中没有 `.clang-tidy` 时，会运行一小组默认检查。

```case
tests/code_actions/fix/02_tidy_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**来自附注（note）的修复**

每条附带修复的附注都会将其作为快速修复提供，标题取自该附注

```case
tests/code_actions/fix/03_note_fixes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 格式化

若项目中有适用于当前文件的 clang-format 样式，生成的文本会按该样式格式化，使生成的函数定义或一组 `case` 标签契合周围代码的排版。

<!-- BEGIN GENERATED ITEMS: code_actions/formatting -->

<!-- BEGIN CAPABILITY: supported -->

**项目格式化样式**

生成的代码遵循适用于当前文件的 clang-format 样式

若文件上层目录中没有 `.clang-format`，或设置了 `DisableFormat`，生成的文本则保留其他示例中展示的排版。

```case
tests/code_actions/formatting/01_project_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 已知限制

- 放入宿主源文件的定义会排在索引已知的、该文件中该类成员的最后一个定义之后；若文件中尚无任何成员定义，则放在文件末尾。哪个源文件充当头文件的宿主，取决于该头文件的编译上下文。
- 已在另一个源文件中定义的成员，只有在项目索引记录了该定义时才会从“定义缺失的成员”操作中排除；只要索引尚未收录定义它们的文件，所有未定义的成员仍会全部列出。
- 指代依赖基类（dependent base class）成员的返回类型会按原样复制到类外定义中，在类外可能需要补上 `typename` 和基类限定符。
- 缺失头文件的候选来自标准库映射以及项目索引见过的头文件；未被任何已索引源文件包含过的头文件不会出现在建议中。
