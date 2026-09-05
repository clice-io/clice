# Folding Ranges

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/folding_range/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

## Fold Kinds

<!-- BEGIN GENERATED ITEMS: fold_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Block folding**

functions, classes, structs, unions, enums, namespaces, lambdas

```snap
tests/snap/folding_range/fold_kinds/01_block_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested compound-statement folding**

`if`/`for`/`while` bodies inside functions

```snap
tests/snap/folding_range/fold_kinds/02_nested_compound_statement.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Multi-line list folding**

Function parameters, call arguments, initializer lists, lambda captures

```snap
tests/snap/folding_range/fold_kinds/03_multiline_list_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1455 -->

**Access-specifier section folding**

`public:` / `protected:` / `private:` regions within a class

```snap
tests/snap/folding_range/fold_kinds/04_access_specifier_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1661 clangd#2059 -->

**Preprocessor conditional folding (`#if` / `#ifdef` / `#ifndef` ... `#endif`)**

Branch regions delimited by `#else` fold today; a bare `#if ... #endif`
block without an `#else` does not fold yet. clangd#2059 is a duplicate
of clangd#1661.

```snap
tests/snap/folding_range/fold_kinds/05_preprocessor_conditional.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1623 -->

**Custom region folding (`#pragma region` / `#pragma endregion`)**

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

<!-- BEGIN CAPABILITY: unsupported -->

**Comment folding**

multi-line `/* */` and consecutive `//` line comments

```snap
tests/snap/folding_range/fold_kinds/08_comment_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Include region folding**

Consecutive `#include` directives

```snap
tests/snap/folding_range/fold_kinds/09_include_region.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Raw string literal folding**

```snap
tests/snap/folding_range/fold_kinds/10_raw_string_literal.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`using` declaration blocks**

Consecutive using declarations/directives

```snap
tests/snap/folding_range/fold_kinds/11_using_declaration_block.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Template parameter list folding**

```snap
tests/snap/folding_range/fold_kinds/12_template_parameter_list.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template specializations and instantiations**

Written specializations and their members fold; instantiated declarations reuse the pattern's source locations and must not fold it again

```snap
tests/snap/folding_range/fold_kinds/13_template_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Abbreviated function templates**

Bodies of functions with `auto` or constrained `auto` parameters fold like any other function

```snap
tests/snap/folding_range/fold_kinds/14_abbreviated_function_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-generated folding**

Braces and access specifiers spelled through macros fold at the invocation site

```snap
tests/snap/folding_range/fold_kinds/15_macro_folding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Coroutine bodies**

The written block folds exactly once and the coroutine transformation wrapper adds no duplicate fold; a coroutine lambda keeps its body fold

```snap
tests/snap/folding_range/fold_kinds/16_coroutine_body.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Initializer-list constructions**

The constructor's braces and the nested initializer list share delimiters and fold once; a parenthesized list argument keeps both folds

```snap
tests/snap/folding_range/fold_kinds/17_initializer_list_construction.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Refinements

<!-- BEGIN GENERATED ITEMS: refinements -->

<!-- BEGIN CAPABILITY: supported clangd#2667 -->

**`collapsedText` placeholder (LSP 3.17)**

Show a summary when folded

> **Client support**: VS Code does **not** support `collapsedText` yet
> ([vscode#70794](https://github.com/microsoft/vscode/issues/70794) — still
> open); Neovim with nvim-lsp supports it natively. Clients that do not
> implement this field will silently ignore it — the folding still works,
> only the placeholder text is missing.

```snap
tests/snap/folding_range/refinements/01_collapsed_text.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2666 -->

**Fold from the declaration line for function/class bodies**

Keep the signature visible when folded

> **Client support**: this depends on the client interpreting
> `FoldingRange.startLine` correctly. VS Code uses the line _after_
> `startLine` as the first hidden line, so setting `startLine` to the
> declaration line achieves the desired effect. However, VS Code still
> leaves the closing `}` on a separate line rather than collapsing it onto
> the signature line ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)
> — still open). Other clients may differ.

```snap
tests/snap/folding_range/refinements/02_fold_from_declaration_line.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Inactive preprocessor branch indication**

Visually distinguish or auto-fold inactive `#if`/`#else` branches

The server emits a fold range for the region between the condition and
`#else`, so the first branch can be folded manually; the post-`#else`
branch gets no range yet. Knowing which branch is _inactive_ — to dim or
auto-fold it — is not implemented here; that information belongs to the
inactive-regions feature.

> **Note**: this overlaps with semantic tokens (inactive code dimming) and
> is partly a client UX concern. The server can mark these ranges with
> `FoldingRangeKind.Region` and clients can choose to auto-fold them.

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
