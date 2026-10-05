# Folding Ranges

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

## Fold Kinds

<!-- BEGIN GENERATED ITEMS: fold_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Block folding**

Functions, types, namespaces and lambdas form folding ranges

```snap
tests/snap/folding_range/fold_kinds/01_block_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested compound-statement folding**

Nested control-flow bodies form folding ranges

```snap
tests/snap/folding_range/fold_kinds/02_nested_compound_statement.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Multi-line list folding**

Multiline parameter, argument, initializer and capture lists form folding
ranges

```snap
tests/snap/folding_range/fold_kinds/03_multiline_list_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**Access-specifier section folding**

Access-specifier regions within a class form folding ranges

```snap
tests/snap/folding_range/fold_kinds/04_access_specifier_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1661 clangd#2059 -->

**Preprocessor conditional folding**

Each branch of a conditional forms a folding range up to the directive that
ends it, which stays visible

```snap
tests/snap/folding_range/fold_kinds/05_preprocessor_conditional.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**Pragma region folding**

Named pragma regions form folding ranges

```snap
tests/snap/folding_range/fold_kinds/06_pragma_region.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pragma classification**

Only the first argument token decides region/endregion

```snap
tests/snap/folding_range/fold_kinds/07_pragma_classification.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Comment folding**

Multiline block comments and runs of line comments fold

Line comments on consecutive lines fold as one run below the first line,
which stays visible; a blank line or a line of code ends the run. A block
comment folds on its delimiters like a brace pair. A comment trailing code
does not fold.

```snap
tests/snap/folding_range/fold_kinds/08_comment_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include region folding**

Consecutive include directives fold as one run below the first include

A blank line, a comment line or another directive ends the run; includes
inside a conditional branch fold within it, whether or not the branch is
taken.

```snap
tests/snap/folding_range/fold_kinds/09_include_region/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Raw string literal folding**

Multiline raw string literals fold on their delimiters

The placeholder repeats the encoding prefix, a custom delimiter and a
literal suffix. A raw string written in a macro argument folds where it is
written.

```snap
tests/snap/folding_range/fold_kinds/10_raw_string_literal.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`using` declaration blocks**

Consecutive using declarations and directives fold below the first one

A blank line or any other line ends the run, and alias declarations do not
join one, nor does a declaration sharing its line with other code. Using
declarations produced by macros fold at the invocations.

```snap
tests/snap/folding_range/fold_kinds/11_using_declaration_block.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template parameter list folding**

Multiline template parameter lists fold on their angle brackets

Class, function, variable and alias templates, partial specializations,
the lists an out-of-line member definition repeats, template template
parameters and lambdas with explicit template parameters all fold their
parameter lists.

```snap
tests/snap/folding_range/fold_kinds/12_template_parameter_list.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template specializations and instantiations**

Written specializations and their members fold; instantiated declarations
reuse the pattern's source locations and do not fold it again

```snap
tests/snap/folding_range/fold_kinds/13_template_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Abbreviated function templates**

Bodies of functions with `auto` or constrained `auto` parameters fold like
any other function

```snap
tests/snap/folding_range/fold_kinds/14_abbreviated_function_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-generated folding**

Braces and access specifiers spelled through macros fold at the invocation
site

```snap
tests/snap/folding_range/fold_kinds/15_macro_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Coroutine bodies**

The written block folds exactly once and the coroutine transformation
wrapper adds no duplicate fold; a coroutine lambda keeps its body fold

```snap
tests/snap/folding_range/fold_kinds/16_coroutine_body.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Initializer-list constructions**

The constructor's braces and the nested initializer list share delimiters
and fold once; a parenthesized list argument keeps both folds

```snap
tests/snap/folding_range/fold_kinds/17_initializer_list_construction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-argument folding**

Code written inside macro arguments folds where it is written

```snap
tests/snap/folding_range/fold_kinds/18_macro_argument_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Linkage specification blocks**

`extern "C"` blocks form folding ranges, also behind the usual
`__cplusplus` guards

```snap
tests/snap/folding_range/fold_kinds/19_linkage_specification.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module fragments and export blocks**

The global and private module fragments and `export` blocks form folding
ranges

```snap
tests/snap/folding_range/fold_kinds/20_module_blocks.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Refinements

<!-- BEGIN GENERATED ITEMS: refinements -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**`collapsedText` placeholder (LSP 3.17)**

Folded ranges can show a summary

> **Client support**: VS Code does **not** support `collapsedText` yet
> ([vscode#70794](https://github.com/microsoft/vscode/issues/70794) — still
> open); Neovim with nvim-lsp supports it natively. Clients that do not
> implement this field will silently ignore it — the folding still works,
> only the placeholder text is missing.

```snap
tests/snap/folding_range/refinements/01_collapsed_text.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2666 -->

**Declaration-line folding**

A declaration's body folds from the line holding its name

When the opening brace of a function, class or namespace sits below the
name — on a line of its own, or after a signature spanning several lines —
a client that folds whole lines starts the fold on the name's line, so the
folded declaration keeps showing what it is. A client folding by
characters starts at the brace and keeps everything before it visible
anyway. A conditional directive between the name and the brace keeps the
fold at the brace.

> **Client support**: VS Code still leaves the closing `}` on a separate
> line rather than collapsing it onto the signature line
> ([vscode#3352](https://github.com/microsoft/vscode/issues/3352) — still
> open).

```snap
tests/snap/folding_range/refinements/02_fold_from_declaration_line.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive preprocessor branches**

Untaken branches fold like taken ones

Every branch of a conditional folds whether or not the compile takes it,
conditionals nested in an untaken branch included, so dead code can be
folded away by hand. Untaken code is dimmed by the `inactive` modifier of
semantic tokens; the folds themselves do not tell the branches apart.

```snap
tests/snap/folding_range/refinements/03_inactive_preprocessor_branch.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Single-line constructs stay unfolded**

A fold that hides nothing is noise

```snap
tests/snap/folding_range/refinements/04_single_line_constructs.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
