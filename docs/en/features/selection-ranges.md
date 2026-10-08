# Selection Ranges

Expand and shrink the selection by syntax (`textDocument/selectionRange`): each step grows the selection to the next enclosing construct — an expression, a statement with its semicolon, a declaration, a block — and also stops at what only the text shows, such as the inside of a pair of brackets, the text of a string literal or a comment. A file served in read-only mode, without a compiled AST, gets the text-only steps.

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/selection_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Syntax Ranges

<!-- BEGIN GENERATED ITEMS: syntax_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**Expression nesting**

Each step grows the selection to the next enclosing expression, through
the inside of its parentheses before the parentheses themselves

```snap
tests/snap/selection_range/syntax_ranges/01_expression_nesting.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Statements and declarations**

A statement or declaration is selected with the semicolon that ends it,
after the part before it

A local variable steps from its declarator to the declaration statement.
A member, a global or a function declaration gains its semicolon the
same way; a template's declaration gains it before its `template` head
joins.

```snap
tests/snap/selection_range/syntax_ranges/02_statement_semicolon.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Argument and parameter lists**

The arguments of a call and the parameters of a function are selected
without their parentheses before with them

```snap
tests/snap/selection_range/syntax_ranges/03_argument_lists.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template argument and parameter lists**

Template arguments and template parameters are selected inside their angle
brackets before with them

```snap
tests/snap/selection_range/syntax_ranges/04_template_lists.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1324 -->

**Macro arguments**

A name or expression written in a macro argument is selected where it is
written, then the argument list and the whole invocation

Code a macro's replacement spells around the argument has no place in
the file of its own: the steps go from the argument straight to the
invocation.

```snap
tests/snap/selection_range/syntax_ranges/05_macro_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambdas**

A lambda's capture list, parameter list and body are steps of their own on
the way to the whole lambda

```snap
tests/snap/selection_range/syntax_ranges/06_lambdas.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Blocks and their contents**

The statements of a block, the members of a class and the declarations of
a namespace are selected without their braces before with them

A cursor on a blank line inside a block starts from the block's
contents.

```snap
tests/snap/selection_range/syntax_ranges/07_blocks.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Text Ranges

<!-- BEGIN GENERATED ITEMS: text_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**String and character literals**

The text of a string or character literal is selected without its quotes
before with them

An encoding prefix, a raw string's delimiter and a user-defined suffix
stay outside the text.

```snap
tests/snap/selection_range/text_ranges/01_literals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

A comment's text is selected without its markers, then the comment, then
the run of comment lines it belongs to

```snap
tests/snap/selection_range/text_ranges/02_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Preprocessor directives**

A directive is selected as a whole line, after the header name it includes
or the brackets of its macro body

```snap
tests/snap/selection_range/text_ranges/03_directives/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
