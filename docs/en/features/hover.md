# Hover

Rich information cards for the symbol under the cursor.

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Symbol Information

<!-- BEGIN GENERATED ITEMS: symbol_information -->

<!-- BEGIN CAPABILITY: supported -->

**Qualified name**

The hover card shows the enclosing namespace and class scope

```snap
tests/snap/hover/symbol_information/01_qualified_name.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Symbol kind**

The card names what the symbol is: struct, enum, function, field, …

```snap
tests/snap/hover/symbol_information/02_symbol_kind.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Access specifier**

Members show their public / protected / private access

```snap
tests/snap/hover/symbol_information/03_access_specifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definition rendering**

The card includes the symbol's source definition

```snap
tests/snap/hover/symbol_information/04_definition_rendering.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Initializer truncation**

Huge initializers render truncated, not in full

The rendered definition omits the initializer, but the evaluated
`Value` field still spells out all 256 elements.

```snap
tests/snap/hover/symbol_information/05_initializer_truncation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2474 -->

**Virtual modifiers**

`virtual` / `override` / `final` show on method hover

Modifiers written in the source render (`virtual … = 0`, `override`,
`final`), but an overriding method that omits the redundant `virtual`
keyword gives no sign of its virtuality — the card lacks the
`virtual void draw() override` form the issue asks for.

```snap
tests/snap/hover/symbol_information/06_virtual_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#436 -->

**Anonymous namespace scope**

`(anonymous namespace)` shows in the scope display

The cards render, but the anonymous segment is dropped from the
scope display: a top-level anonymous member shows no scope line at
all, and `outer::(anonymous)` shows just `outer`.

```snap
tests/snap/hover/symbol_information/07_anon_namespace_scope.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Information

<!-- BEGIN GENERATED ITEMS: type_information -->

<!-- BEGIN CAPABILITY: supported -->

**Variable types**

Pointers, references, arrays

A variable's card pretty-prints its declared type, spelling the pointer,
reference and array declarators the way they read in source.

```snap
tests/snap/hover/type_information/01_variable_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

The desugared `aka` form

A sugared type shows its underlying type as `Alias (aka int)`. The
`show_aka` option turns the `aka` suffix off.

```snap
tests/snap/hover/type_information/02_aka_desugar.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function signatures**

Return type, parameter names, defaults

A function's card lists its return type, each parameter with its name,
and any default argument.

```snap
tests/snap/hover/type_information/03_function_signature.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template parameters**

Type, template-template, non-type

Each template parameter kind reports its form: a type parameter, a
template-template parameter, and a non-type parameter with its default.

```snap
tests/snap/hover/type_information/04_template_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` deduction**

The type the placeholder resolves to

Hovering an `auto` placeholder shows the type substituted for it —
builtins, pointers, lambdas, template instantiations, and the
`/* not deduced */` marker inside an uninstantiated template.

```snap
tests/snap/hover/type_information/05_auto_deduction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` deduction**

Value, reference and dependent forms

Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
type, including the reference the parenthesized-expression rule adds.

```snap
tests/snap/hover/type_information/06_decltype_deduction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#435 -->

**CTAD**

Deduced template arguments of a class placeholder

With class template argument deduction the variable's card shows the
deduced `Box<int>`, but hovering the class-name spelling still reports
the primary template without its arguments.

```snap
tests/snap/hover/type_information/07_ctad_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#230 -->

**Instantiation arguments**

Template parameters bound at a use site

A use of a template shows the substituted types (`Wrapper<int>`,
`identity<int>`, `int x`), but not an explicit `T = int` mapping of each
parameter to the argument it was bound to.

```snap
tests/snap/hover/type_information/08_instantiation_args.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#493 -->

**Lambda `auto` parameters**

Deduced parameter type

Hovering the `auto` parameter of a generic lambda yields no card; the
deduced parameter type is not shown.

```snap
tests/snap/hover/type_information/09_lambda_auto_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Sugared `auto`**

Alias sugar preserved through deduction

clangd tracks lost alias sugar through `auto` as clangd#709; clice
already keeps the alias spelling and appends its desugared form, so
`auto` deduced from an aliased return type reads as `Outer // aka: int`.

```snap
tests/snap/hover/type_information/10_sugared_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2156 -->

**Type formatting**

Clang-format applied to rendered types

Long or nested types are printed by the compiler's default type printer;
they are not re-wrapped or aligned through clang-format.

```snap
tests/snap/hover/type_information/11_clang_format_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2219 -->

**Anonymous struct typedef**

The classic C `typedef struct {…} Name`

Compiled as C11: clangd renders a misleading `struct Point` for the
alias of an anonymous struct; clice names the struct after its typedef,
so both the alias and a variable of it report a clean `Point` card.

```snap
tests/snap/hover/type_information/12_c_typedef_anon.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Concept constraints**

The constraint behind a parameter or `auto` placeholder

The constrained-parameter and concept-reference cards carry the
constraint, but hovering the placeholder of a constrained `Addable auto`
variable shows only the deduced type — the constraint is dropped.

```snap
tests/snap/hover/type_information/13_concept_constraints.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Layout Information

<!-- BEGIN GENERATED ITEMS: layout_information -->

<!-- BEGIN CAPABILITY: supported -->

**Field layout**

Size, offset, alignment and padding show on field hover

```snap
tests/snap/hover/layout_information/01_field_layout.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1763 -->

**Type-level layout**

Hovering the type itself shows its size, alignment and padding

Size and alignment show on the type card today; the total padding
does not yet.

```snap
tests/snap/hover/layout_information/02_type_layout.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1771 -->

**Vtable offset**

Virtual methods show their table slot

The method card renders without any vtable fact today.

```snap
tests/snap/hover/layout_information/03_vtable_offset.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Expression Context

<!-- BEGIN GENERATED ITEMS: expression_context -->

<!-- BEGIN CAPABILITY: supported -->

**Constant evaluation**

Constexpr, enumerators, sizeof

When an initializer is a constant expression, the card evaluates it and
shows the resulting value.

```snap
tests/snap/hover/expression_context/01_constant_value.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call arguments**

Which parameter each argument binds to

Hovering an argument at a call site shows the parameter it is passed to,
naming the parameter it binds.

```snap
tests/snap/hover/expression_context/02_callee_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pass semantics**

By value, by reference, by const reference

The argument card states how the value reaches the callee: copied by
value, or bound to a mutable or const reference parameter.

```snap
tests/snap/hover/expression_context/03_pass_semantics.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Implicit conversions**

Argument converted to the parameter type

When an argument reaches a parameter through an implicit conversion, the
card notes the target type, for both built-in and user-defined
conversions.

```snap
tests/snap/hover/expression_context/04_implicit_conversion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1016 -->

**String literals**

The length reported on hover

A string-literal card reports the array type and its size in bytes
(`const char[6]`, `Size: 6 bytes` — the length plus the null
terminator), not an explicit character count.

```snap
tests/snap/hover/expression_context/05_string_length.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1669 -->

**Numeric literals**

Type and value of an integer or float literal

Hovering a numeric literal yields no card, unlike character and string
literals, whose type and value are shown.

```snap
tests/snap/hover/expression_context/06_numeric_literal_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1622 -->

**Record variables**

Enclosing constant value leaks in

Hovering a record-typed argument of a constant-evaluable call currently
reports that call's value (`Value = 7`) on the variable — a value that
is not the record's own.

```snap
tests/snap/hover/expression_context/07_record_value_misleading.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Documentation

<!-- BEGIN GENERATED ITEMS: documentation -->

<!-- BEGIN CAPABILITY: supported -->

**Doxygen `///` comments**

Extracted from the declaration and rendered on hover

Applies to plain functions, primary templates and their specializations;
a reference resolves to the most specialized declaration's comment.

```snap
tests/snap/hover/documentation/01_doxygen_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Synthesized accessor docs**

Trivial getters/setters get a generated one-line description

A trivial getter or setter with no comment of its own gets a synthesized
"Trivial accessor/setter for `field`." line in its hover card.

```snap
tests/snap/hover/documentation/02_accessor_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1320 -->

**`@copydoc` tags**

Copy another symbol's documentation onto this one

A `@copydoc target` tag should copy `target`'s documentation into this
symbol's hover card. clice does not resolve the tag yet — the card shows
the literal `@copydoc base_func()` text.

```snap
tests/snap/hover/documentation/03_copydoc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2504 -->

**Inherited override docs**

An override with no comment shows the base method's documentation

Hovering an overriding method that carries no comment of its own should
surface the documentation from the method it overrides. clice does not
inherit it yet — the override's card carries no description.

```snap
tests/snap/hover/documentation/04_inherit_overridden_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2506 -->

**Overload doc sharing**

A later overload with no comment reuses the first overload's documentation

Consecutive overloads often document only the first; a later undocumented
overload should reuse that shared description. clice does not share it
yet — the later overload's card carries no description.

```snap
tests/snap/hover/documentation/05_overload_doc_sharing.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1936 -->

**Inherited constructor docs**

`using Base::Base;` surfaces the base constructor's documentation

A constructor pulled in with `using Base::Base;` should carry the base
constructor's documentation on hover. There is no hover surface for it:
the name in the using-declaration resolves to the class, not the
inherited constructor.

```snap
tests/snap/hover/documentation/06_inherited_ctor_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#974 -->

**Banner comments**

A section banner separated by a blank line must not attach to the next declaration

A `// ==== Section ====` banner followed by a blank line should not be
misattributed as documentation for the declaration below it. clice
currently attaches it anyway — the banner text appears in the card.

```snap
tests/snap/hover/documentation/07_comment_association.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration vs definition comments**

The declaration's doc wins over a definition-site comment

clangd tracks this as clangd#829; clice already prefers the
declaration's `///` documentation over the definition's plain `//` note,
showing it at both the declaration and the definition site.

```snap
tests/snap/hover/documentation/08_decl_vs_def_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2057 -->

**Whitespace and newlines**

A markdown table in a comment keeps its line breaks

A markdown table written across several `///` lines should render as a
table with its line breaks preserved. clice currently flattens the lines
onto one line, so the table does not render.

```snap
tests/snap/hover/documentation/09_whitespace_preserve.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1040 -->

**Comment indentation**

Indented lines in a comment render without spurious extra indentation

A doc comment whose body contains an indented block should render with
correct indentation. clice currently strips the leading indentation, so
an indented code block loses its offset and the blank line collapses.

```snap
tests/snap/hover/documentation/10_comment_indentation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1226 -->

**Template keyword from a macro**

The docstring should survive the expansion

When the `template` keyword is produced by a macro expansion, the
declaration's doc comment should still appear on hover. clice currently
drops it — the card carries no description.

```snap
tests/snap/hover/documentation/11_macro_template_doc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2148 -->

**Comment suppression option**

A config switch to hide misattributed doc comments

A stray comment picked up by the association heuristic — a section
banner separated from the code by a blank line, for example — always
reaches the hover card: clice has no config option to suppress doc
comments whose attachment is a guess.

```snap
tests/snap/hover/documentation/12_comment_suppression.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macro Hover

<!-- BEGIN GENERATED ITEMS: macro_hover -->

<!-- BEGIN CAPABILITY: supported -->

**Definition text at every site**

`#define`, use, `#ifdef` and `#undef` all show the macro's definition

A macro's hover card carries its `#define` text wherever the name
appears: the definition itself, a use, an `#ifdef` guard and an `#undef`.

```snap
tests/snap/hover/macro_hover/01_macro_definition_sites.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Fully-expanded preview**

A function-like macro use shows its arguments substituted through the body

Hovering a function-like macro invocation shows the `#define` text and a
preview of the fully-expanded result with the call's arguments spliced in.

```snap
tests/snap/hover/macro_hover/02_expansion_preview.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Command-line macros**

`-D` definitions hover with a synthesized `#define`

A macro defined on the command line (`-DFROM_CLI=7`) shows a synthesized
`#define FROM_CLI 7` in its hover card, then its expansion.

```snap
tests/snap/hover/macro_hover/03_cli_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Nested macro in arguments**

A macro named inside another invocation's arguments

The expansion preview starts at the outer invocation, so hovering an
inner macro named inside the arguments shows only its definition, not an
expansion preview.

```snap
tests/snap/hover/macro_hover/04_nested_arg_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2642 -->

**Use before definition**

Hovering a macro name that appears before its `#define`

A macro name used in an `#if` above its own `#define` should still hover
with the macro's definition. clice currently returns no hover at the
pre-definition use; a use after the `#define` works normally.

```snap
tests/snap/hover/macro_hover/05_expansion_before_definition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`#define` inside the preamble**

Hover on a leading directive

A `#define` in the leading run of directives before the first declaration
has no hover card, while definitions after a declaration do.

```snap
tests/snap/hover/macro_hover/06_preamble_define_hover.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Special Hover Targets

<!-- BEGIN GENERATED ITEMS: special_hover_targets -->

<!-- BEGIN CAPABILITY: partial clangd#959 -->

**Members on type hover**

Hovering an enum or struct type lists its members

The card names the type (and a struct's layout), but the member list is
not expanded — the body renders as `{}`.

```snap
tests/snap/hover/special_hover_targets/01_type_members_on_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2020 -->

**Typedef underlying struct**

Hovering an alias expands the aliased definition

The card resolves the alias to its underlying type name, but does not
expand that struct's definition or member list.

```snap
tests/snap/hover/special_hover_targets/02_typedef_underlying.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1862 -->

**Keyword documentation**

Hovering a language keyword shows its description

Hovering a keyword such as `const` or `virtual` produces no card.

```snap
tests/snap/hover/special_hover_targets/03_keyword_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Attribute documentation**

Hovering an attribute shows its description

The attribute's own documentation renders in the card, for both GNU
`__attribute__` spellings and C++ `[[...]]` attributes.

```snap
tests/snap/hover/special_hover_targets/04_attributes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include directive hover**

Hovering an `#include` shows the resolved header path

The card resolves the quoted header to its file on disk.

```snap
tests/snap/hover/special_hover_targets/05_include_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this` expression**

Hovering `this` shows the pointed-to class type

Works in a plain class and inside a class template.

```snap
tests/snap/hover/special_hover_targets/06_this_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Predefined identifiers**

`__func__` hover shows the current function name

The value resolves in a concrete function; inside a template only the
approximate type is known.

```snap
tests/snap/hover/special_hover_targets/07_predefined_identifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**No hover on meaningless tokens**

Builtin keywords and empty bodies yield no card

Hovering a builtin type keyword or the inside of an empty body
produces no card at all, so editors show nothing rather than noise.
(Numeric and bool literals also have no card today, but that is a
tracked gap — see the numeric-literal item — not a promise.)

```snap
tests/snap/hover/special_hover_targets/08_no_hover_negatives.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2662 -->

**GTK-Doc and kernel-doc**

Recognize GObject Introspection annotations

GTK-Doc / kernel-doc comment syntax and GObject Introspection
annotations are not parsed into the hover card.

```snap
tests/snap/hover/special_hover_targets/09_gtk_doc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2669 -->

**LaTeX math in Doxygen**

Inline Doxygen formulas appear verbatim

The formula text is not rendered as math.

```snap
tests/snap/hover/special_hover_targets/10_latex_math.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: presentation -->

<!-- BEGIN CAPABILITY: supported -->

**Markdown rendering**

Cards render as markdown, or plain text via `parse_comment_as_markdown = false`

```snap
tests/snap/hover/presentation/01_presentation.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module-Related

<!-- BEGIN GENERATED ITEMS: module_related -->

<!-- BEGIN CAPABILITY: unsupported -->

**Import statement hover**

Hovering `import` shows the module's info

Hovering an `import` declaration does not yet describe the imported
module.

```snap
tests/snap/hover/module_related/01_import_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module name hover**

Hovering a module name lists its owning files

Hovering a module name does not yet list the files or partitions that
declare it.

```snap
tests/snap/hover/module_related/02_module_name_hover.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Hover Correctness

Robustness on inputs that have broken other tooling.

<!-- BEGIN GENERATED ITEMS: hover_correctness -->

<!-- BEGIN CAPABILITY: supported -->

**MSVC inheritance model**

`MSInheritanceAttr` does not corrupt record hover

clangd tracks this as clangd#1643 and clangd#2212; under an MSVC target
the implicit inheritance attribute does not leak into the record or
method card.

```snap
tests/snap/hover/hover_correctness/01_ms_inheritance.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Most-vexing-parse**

Object init and function declaration hover distinctly

clangd tracks this as clangd#2225; clice reads the direct-init as a
variable and the vexing form as a function declaration.

```snap
tests/snap/hover/hover_correctness/02_object_vs_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Large unsigned enum constant**

Hovering a `0xFFFF...ULL` enumerator does not crash

clangd crashes on this (clangd#2381); clice renders the full unsigned
value without overflow.

```snap
tests/snap/hover/hover_correctness/03_large_enum_value.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call with default arguments**

Hovering a call that omits defaults does not crash

clangd crashes on this (clangd#551); clice renders the callee signature
with its default arguments.

```snap
tests/snap/hover/hover_correctness/04_default_args_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-shadowed symbol**

A function-like macro over a same-named function

clangd tracks this as clangd#2490; at the call site the function-like
macro is active, and clice's card shows that macro and its expansion.

```snap
tests/snap/hover/hover_correctness/05_macro_shadowed_symbol.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
