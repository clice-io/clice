# 选择范围

按语法扩大和缩小选区（`textDocument/selectionRange`）：每一步都把选区扩大到外层的下一个构造——表达式、连同分号的语句、声明、代码块——也会停在只有文本才看得出的位置，例如一对括号的内部、字符串字面量或注释的文本。以只读模式提供服务、没有编译出 AST 的文件，只有基于文本的这些步骤。

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/selection_range/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 语法范围

从光标所在的名称开始，按代码结构逐步向外扩大选区。

<!-- BEGIN GENERATED ITEMS: selection_range/syntax_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**嵌套表达式**

选区从光标所在的名称开始，逐步扩大到包围它的各层表达式和语句

```case
tests-next/selection_range/syntax_ranges/01_nested_expression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**语句和声明**

先选中语句或声明中分号前的部分，再将结尾的分号纳入选区

局部变量从声明符扩大到整条声明语句。成员、全局变量或函数的声明以同样的方式纳入分号；模板的声明先纳入分号，再纳入 `template` 头。

```case
tests-next/selection_range/syntax_ranges/02_statements
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**实参和形参列表**

先选中调用的实参列表和函数的形参列表，再将两侧的圆括号纳入选区

```case
tests-next/selection_range/syntax_ranges/03_argument_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模板实参和形参列表**

先选中尖括号内的模板实参列表和模板形参列表，再将尖括号纳入选区

```case
tests-next/selection_range/syntax_ranges/04_template_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1324 -->

**宏实参**

先在宏实参的书写位置选中其中的名称或表达式，再依次选中实参列表和整个宏调用

宏的替换文本在实参周围写出的代码，在文件中没有自己的位置：选区从实参直接扩大到宏调用。

```case
tests-next/selection_range/syntax_ranges/05_macro_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda 表达式**

在选区扩大到整个 Lambda 表达式的过程中，捕获列表、形参列表和函数体各自对应一个选择步骤

```case
tests-next/selection_range/syntax_ranges/06_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**代码块及其内容**

先选中代码块中的语句、类中的成员和命名空间中的声明，再将两侧的花括号纳入选区

光标位于代码块内的空行上时，从代码块的内容开始选择。

```case
tests-next/selection_range/syntax_ranges/07_blocks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文本范围

仅由文本确定的选择步骤：选中字面量引号内的内容、注释的文本或指令所在的行。

<!-- BEGIN GENERATED ITEMS: selection_range/text_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**字符串和字符字面量**

先选中字符串或字符字面量的文本，再将引号纳入选区

编码前缀、原始字符串的定界符和用户定义后缀都不算在文本之内。

```case
tests-next/selection_range/text_ranges/01_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**注释**

先选中不含注释标记的注释文本，再选中整条注释，最后选中它所在的一组连续注释行

```case
tests-next/selection_range/text_ranges/02_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预处理指令**

先选中指令引入的头文件名或宏体中的括号范围，再选中指令所在的整行

```case
tests-next/selection_range/text_ranges/03_directives
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
