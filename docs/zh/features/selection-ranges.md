# 选择范围

按语法扩大和缩小选区（`textDocument/selectionRange`）：每一步都把选区扩大到外层的下一个构造——表达式、连同分号的语句、声明、代码块——也会停在只有文本才看得出的位置，例如一对括号的内部、字符串字面量或注释的文本。以只读模式提供服务、没有编译出 AST 的文件，只有基于文本的这些步骤。

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/selection_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## 语法范围

<!-- BEGIN GENERATED ITEMS: syntax_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**表达式嵌套**

每一步把选区扩大到外层的下一个表达式，先到括号内部，再到括号本身

```snap
tests/snap/selection_range/syntax_ranges/01_expression_nesting.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句和声明**

语句或声明先选中分号之前的部分，再连同结尾的分号一起选中

局部变量从声明符扩大到整条声明语句。成员、全局变量或函数的声明以同样的方式纳入分号；模板的声明先纳入分号，再纳入 `template` 头。

```snap
tests/snap/selection_range/syntax_ranges/02_statement_semicolon.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**实参和形参列表**

调用的实参和函数的形参先不带括号选中，再连同括号选中

```snap
tests/snap/selection_range/syntax_ranges/03_argument_lists.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板实参和形参列表**

模板实参和模板形参先在尖括号以内选中，再连同尖括号选中

```snap
tests/snap/selection_range/syntax_ranges/04_template_lists.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1324 -->

**宏实参**

写在宏实参中的名字或表达式先在其书写位置选中，然后是实参列表，最后是整个宏调用

宏的替换文本在实参周围写出的代码，在文件中没有自己的位置：选区从实参直接扩大到宏调用。

```snap
tests/snap/selection_range/syntax_ranges/05_macro_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 表达式**

Lambda 的捕获列表、形参列表和函数体在扩大到整个 Lambda 之前各占一步

```snap
tests/snap/selection_range/syntax_ranges/06_lambdas.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**代码块及其内容**

代码块中的语句、类的成员和命名空间中的声明先不带大括号选中，再连同大括号选中

光标位于代码块内的空行上时，从代码块的内容开始选择。

```snap
tests/snap/selection_range/syntax_ranges/07_blocks.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文本范围

<!-- BEGIN GENERATED ITEMS: text_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**字符串和字符字面量**

字符串或字符字面量的文本先不带引号选中，再连同引号选中

编码前缀、原始字符串的定界符和用户定义后缀都不算在文本之内。

```snap
tests/snap/selection_range/text_ranges/01_literals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

先选中不含注释标记的注释文本，再选中整条注释，然后是它所在的那一串连续注释行

```snap
tests/snap/selection_range/text_ranges/02_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预处理指令**

先选中指令所包含的头文件名或其宏体中的括号，再选中整行指令

```snap
tests/snap/selection_range/text_ranges/03_directives/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
