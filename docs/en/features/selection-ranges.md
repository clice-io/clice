# Selection Ranges

Expand and shrink the selection by syntax (`textDocument/selectionRange`): each step grows the selection to the next enclosing construct — an expression, a statement with its semicolon, a declaration, a block — and also stops at what only the text shows, such as the inside of a pair of brackets, the text of a string literal or a comment. A file served in read-only mode, without a compiled AST, gets the text-only steps.

<!-- The cards in the sections below are generated from the documented cases
     in tests/selection_ranges/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Syntax Ranges

Steps that follow the structure of the code, from the name under the cursor outward.

<!-- BEGIN GENERATED ITEMS: selection_ranges/syntax_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**Nested expressions**

Selection grows from the name under the cursor through each enclosing expression and statement

```case
tests/selection_ranges/syntax_ranges/01_nested_expression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Statements and declarations**

A statement or declaration is selected with the semicolon that ends it, after the part before it

A local variable steps from its declarator to the declaration statement.
A member, a global or a function declaration gains its semicolon the
same way; a template's declaration gains it before its `template` head
joins.

```case
tests/selection_ranges/syntax_ranges/02_statements
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Argument and parameter lists**

The arguments of a call and the parameters of a function are selected without their parentheses before with them

```case
tests/selection_ranges/syntax_ranges/03_argument_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template argument and parameter lists**

Template arguments and template parameters are selected inside their angle brackets before with them

```case
tests/selection_ranges/syntax_ranges/04_template_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1324 -->

**Macro arguments**

A name or expression written in a macro argument is selected where it is written, then the argument list and the whole invocation

Code a macro's replacement spells around the argument has no place in
the file of its own: the steps go from the argument straight to the
invocation.

```case
tests/selection_ranges/syntax_ranges/05_macro_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambdas**

A lambda's capture list, parameter list and body are steps of their own on the way to the whole lambda

```case
tests/selection_ranges/syntax_ranges/06_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Blocks and their contents**

The statements of a block, the members of a class and the declarations of a namespace are selected without their braces before with them

A cursor on a blank line inside a block starts from the block's
contents.

```case
tests/selection_ranges/syntax_ranges/07_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function signatures**

A function's name is selected with its signature before the whole declaration

The signature runs from the return type, or from the name of a
constructor, through the parameters and what the declaration adds after
them, such as `const noexcept`, a trailing return type, `override` or a
`requires` clause.

```case
tests/selection_ranges/syntax_ranges/11_function_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Switch cases**

A `case` label is selected with the statements it leads before the whole switch body

```case
tests/selection_ranges/syntax_ranges/15_switch_cases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Documented declarations**

A declaration is selected with the comment documenting it, from inside either one

Inside a block comment over several lines, the line under the cursor
comes first, without the `*` it begins with.

```case
tests/selection_ranges/syntax_ranges/19_documented_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Text Ranges

Steps that only the text shows: the inside of a literal's quotes, the text of a comment, the line of a directive.

<!-- BEGIN GENERATED ITEMS: selection_ranges/text_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**String and character literals**

The text of a string or character literal is selected without its quotes before with them

An encoding prefix, a raw string's delimiter and a user-defined suffix
stay outside the text.

```case
tests/selection_ranges/text_ranges/01_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

A comment's text is selected without its markers, then the comment, then the run of comment lines it belongs to

```case
tests/selection_ranges/text_ranges/02_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Preprocessor directives**

A directive is selected as a whole line, after the header name it includes or the brackets of its macro body

```case
tests/selection_ranges/text_ranges/03_directives
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Conditional Compilation

Steps through the code of `#if` blocks, the branches the preprocessor skipped included.

<!-- BEGIN GENERATED ITEMS: selection_ranges/conditionals -->

<!-- BEGIN CAPABILITY: supported -->

**Conditional blocks**

The lines of a conditional's branch and the whole `#if` through `#endif` are selected after the code inside them

A branch the preprocessor skipped gets the same steps, so its code can
be selected as a whole.

```case
tests/selection_ranges/conditionals/03_conditional_blocks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
