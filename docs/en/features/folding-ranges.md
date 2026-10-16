# Folding Ranges

<!-- The cards in the sections below are generated from the documented cases
     in tests/folding_range/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Blocks

Braces fold: namespaces, types, functions, lambdas and the bodies of statements.

<!-- BEGIN GENERATED ITEMS: folding_range/blocks -->

<!-- BEGIN CAPABILITY: supported -->

**Brace blocks**

Namespaces, types, functions and lambdas fold at their braces

```case
tests/folding_range/blocks/01_braces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Statement bodies**

The bodies of control-flow statements and bare blocks fold

Each branch of an `if`/`else` or a `try`/`catch` folds on its own: the line
that closes one branch and opens the next stays visible.

```case
tests/folding_range/blocks/03_statement_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Linkage specification blocks**

`extern "C"` blocks form folding ranges, also behind the usual `__cplusplus` guards

```case
tests/folding_range/blocks/04_linkage_specifications
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Lists

Any parentheses, brackets or braces spanning lines fold, and so do template parameter and argument lists.

<!-- BEGIN GENERATED ITEMS: folding_range/lists -->

<!-- BEGIN CAPABILITY: supported -->

**Multi-line lists**

Multiline parameter, argument, initializer and capture lists form folding ranges

```case
tests/folding_range/lists/01_multiline_lists
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template parameter lists**

Multiline template parameter lists fold on their angle brackets

Class, function, variable and alias templates, partial specializations,
the lists an out-of-line member definition repeats, template template
parameters and lambdas with explicit template parameters all fold their
parameter lists.

```case
tests/folding_range/lists/02_template_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Every bracket pair**

Any parentheses, brackets or braces spanning lines fold, whatever writes them

Conditions, casts, `static_assert`, parenthesized expressions, subscripts,
attribute lists, function types and the arguments of a macro invocation fold
like a call's arguments do. An attribute list written `[[...]]` folds once.

```case
tests/folding_range/lists/05_every_bracket_pair
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Sections

The members under an access specifier, and the statements under a `case` label, fold as a section up to the next label.

<!-- BEGIN GENERATED ITEMS: folding_range/sections -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**Access-specifier sections**

Access-specifier regions within a class form folding ranges

```case
tests/folding_range/sections/01_access_specifiers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Switch cases**

Each case of a switch folds up to the next label, which stays visible

Labels written one after another fold as one case. A case whose body is a
block opening on the label's line folds as that block.

```case
tests/folding_range/sections/02_switch_cases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Comments and Runs

A block comment folds on its delimiters, and consecutive line comments, includes, imports or `using` declarations fold as one run.

<!-- BEGIN GENERATED ITEMS: folding_range/runs -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

Multiline block comments and runs of line comments fold

Line comments on consecutive lines fold as one run below the first line,
which stays visible; a blank line or a line of code ends the run. A block
comment folds on its delimiters like a brace pair. A comment trailing code
does not fold.

```case
tests/folding_range/runs/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include runs**

Consecutive include directives fold as one run below the first include

Blank lines and comment lines between includes stay in the run; code or
another directive ends it. Includes inside a conditional branch fold within
it, whether or not the branch is taken.

```case
tests/folding_range/runs/02_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`using` declaration runs**

Consecutive using declarations and directives fold below the first one

A blank line or any other line ends the run, and alias declarations do not
join one, nor does a declaration sharing its line with other code. Using
declarations produced by macros fold at the invocations.

```case
tests/folding_range/runs/03_using_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Import runs**

Consecutive import declarations fold as one run, like include directives

`export import` joins the run too.

```case
tests/folding_range/runs/04_import_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Raw Strings

A raw string literal spanning lines folds on its delimiters.

<!-- BEGIN GENERATED ITEMS: folding_range/raw_strings -->

<!-- BEGIN CAPABILITY: supported -->

**Raw string literals**

Multiline raw string literals fold on their delimiters

The placeholder repeats the encoding prefix, a custom delimiter and a
literal suffix. A raw string written in a macro argument folds where it is
written.

```case
tests/folding_range/raw_strings/01_raw_string_literals
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Preprocessor Conditionals

Each branch of a conditional folds up to the directive that ends it, taken or not, and so do the blocks inside an untaken one.

<!-- BEGIN GENERATED ITEMS: folding_range/conditionals -->

<!-- BEGIN CAPABILITY: supported clangd#1661 clangd#2059 -->

**Preprocessor conditionals**

Each branch of a conditional forms a folding range up to the directive that ends it, which stays visible

```case
tests/folding_range/conditionals/01_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Untaken branches**

Untaken branches fold like taken ones

Every branch of a conditional folds whether or not the compile takes it,
conditionals nested in an untaken branch included, so dead code can be
folded away by hand. Untaken code is dimmed by the `inactive` modifier of
semantic tokens; the folds themselves do not tell the branches apart.

```case
tests/folding_range/conditionals/02_untaken_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Blocks in untaken branches**

The blocks of an untaken branch fold too, not only the branch as a whole

A function, a class or a statement body in code the compile skips folds on
its brackets, so dead code can be folded piece by piece.

```case
tests/folding_range/conditionals/05_blocks_in_untaken_branches
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Pragma Regions

`#pragma region` and `#pragma endregion` fold the code between them.

<!-- BEGIN GENERATED ITEMS: folding_range/regions -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**Pragma regions**

Named pragma regions form folding ranges

```case
tests/folding_range/regions/01_pragma_regions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Modules

The module fragments and `export` blocks of a module unit fold.

<!-- BEGIN GENERATED ITEMS: folding_range/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module fragments and export blocks**

The global and private module fragments and `export` blocks form folding ranges

```case
tests/folding_range/modules/01_module_blocks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

Code spelled through a macro, or written in its arguments, folds where you see it.

<!-- BEGIN GENERATED ITEMS: folding_range/macros -->

<!-- BEGIN CAPABILITY: supported -->

**Blocks spelled by macros**

Braces and access specifiers spelled through macros fold at the invocation site

A block a macro opens and closes folds between the two invocations, which
stay visible; its placeholder is `...`.

```case
tests/folding_range/macros/01_macro_spelled_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Code in macro arguments**

Code written inside macro arguments folds where it is written

```case
tests/folding_range/macros/02_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Folding Whole Lines

A client folding whole lines keeps what a reader needs on screen: a body folds from the line holding its declaration's name, and a client showing collapsed text folds the closing line in.

<!-- BEGIN GENERATED ITEMS: folding_range/line_folding -->

<!-- BEGIN CAPABILITY: supported clangd#2666 -->

**Declaration-line folding**

A declaration's body folds from the line holding its name

When the opening brace sits below the name, on a line of its own or after a
signature spanning several lines, a client that folds whole lines starts the
fold on the name's line, so the folded declaration still shows what it is. A
client folding by characters starts at the brace and keeps everything before
it visible anyway.

> **Client support**: VS Code leaves the closing `}` on a line of its own
> rather than joining it to the signature line
> ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)); Neovim
> joins it (see "Closing lines fold in").

```case
tests/folding_range/line_folding/01_declaration_lines
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Closing lines fold in**

A client that shows collapsed text folds a block's closing line too, `int area(Point p) {...}`

Each fold shows its first line followed by `...`; when the closing brace,
`#endif` or `*/` sits on a line of its own, that line folds in and shows
after the `...`. A closing line that opens the next block, such as
`} else {`, stays visible.

> **Client support**: Neovim shows the text through `vim.lsp.foldtext()`.
> VS Code shows no collapsed text, so the closing line stays visible there
> ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)).

```case
tests/folding_range/line_folding/07_collapsed_lines
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Presentation

A folded range shows a placeholder for what it hides, and a fold that would hide nothing is not offered.

<!-- BEGIN GENERATED ITEMS: folding_range/presentation -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**Collapsed text**

A folded range shows a placeholder, such as `{...}` for a body, while the line it starts on stays visible

A client folding by characters, such as Zed, shows the placeholder in place
of the range. A client folding whole lines, such as Neovim, shows its text in
place of the whole first line, so it gets the first line followed by `...`
instead (see "Closing lines fold in").

> **Client support**: VS Code does not show `collapsedText` yet
> ([vscode#70794](https://github.com/microsoft/vscode/issues/70794)). A client
> that ignores it still folds, without the placeholder.

```case
tests/folding_range/presentation/01_collapsed_text
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Single-line constructs stay unfolded**

A fold that hides nothing is noise

```case
tests/folding_range/presentation/02_single_line_constructs
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
