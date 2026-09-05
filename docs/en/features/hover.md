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

<!-- BEGIN CAPABILITY: partial clangd#710 -->

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

Variable hover preserves pointer, reference and array declarators

A variable's card pretty-prints its declared type, spelling the pointer,
reference and array declarators the way they read in source.

```snap
tests/snap/hover/type_information/01_variable_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

Hover can show the desugared `aka` form

A sugared type shows its underlying type as `Alias (aka int)`. The
`show_aka` option turns the `aka` suffix off.

```snap
tests/snap/hover/type_information/02_aka_desugar.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function signatures**

Function hover reports return types, parameter names and defaults

A function's card lists its return type, each parameter with its name,
and any default argument.

```snap
tests/snap/hover/type_information/03_function_signature.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template parameters**

Template hover distinguishes type, template-template and non-type parameters

Each template parameter kind reports its form: a type parameter, a
template-template parameter, and a non-type parameter with its default.

```snap
tests/snap/hover/type_information/04_template_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` deduction**

Placeholder hover shows the type it resolves to

Hovering an `auto` placeholder shows the type substituted for it —
builtins, pointers, lambdas, template instantiations, and the
`/* not deduced */` marker inside an uninstantiated template.

```snap
tests/snap/hover/type_information/05_auto_deduction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` deduction**

Decltype hover distinguishes value, reference and dependent forms

Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
type, including the reference the parenthesized-expression rule adds.

```snap
tests/snap/hover/type_information/06_decltype_deduction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#435 -->

**CTAD**

Class placeholder hover shows its deduced template arguments

With class template argument deduction the variable's card shows the
deduced `Box<int>`, but hovering the class-name spelling still reports
the primary template without its arguments.

```snap
tests/snap/hover/type_information/07_ctad_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#230 -->

**Instantiation arguments**

Template use hover does not show parameter-to-argument bindings yet

A use of a template shows the substituted types (`Wrapper<int>`,
`identity<int>`, `int x`), but not an explicit `T = int` mapping of each
parameter to the argument it was bound to.

```snap
tests/snap/hover/type_information/08_instantiation_args.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#493 -->

**Lambda `auto` parameters**

Generic lambda parameters do not show their deduced type yet

Hovering the `auto` parameter of a generic lambda yields no card; the
deduced parameter type is not shown.

```snap
tests/snap/hover/type_information/09_lambda_auto_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Sugared `auto`**

Alias spelling survives `auto` deduction

Hover keeps the alias spelling and appends its desugared form, so `auto`
deduced from an aliased return type reads as `Outer // aka: int`.

```snap
tests/snap/hover/type_information/10_sugared_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2156 -->

**Type formatting**

Long and nested rendered types are not rewrapped or aligned

Long or nested types are not rewrapped or aligned.

```snap
tests/snap/hover/type_information/11_clang_format_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2219 -->

**Anonymous struct typedef**

C typedef hover names an anonymous struct after its alias

Both the alias and a variable of it report a clean `Point` card rather
than presenting the anonymous type as a separately named struct.

```snap
tests/snap/hover/type_information/12_c_typedef_anon.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Concept constraints**

Constrained placeholders lose their constraint on hover

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

Constant-expression hover reports evaluated values

When an initializer is a constant expression, the card evaluates it and
shows the resulting value.

```snap
tests/snap/hover/expression_context/01_constant_value.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call arguments**

Argument hover identifies its bound parameter

Hovering an argument at a call site shows the parameter it is passed to,
naming the parameter it binds.

```snap
tests/snap/hover/expression_context/02_callee_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pass semantics**

Argument hover distinguishes value and reference passing

The argument card states how the value reaches the callee: copied by
value, or bound to a mutable or const reference parameter.

```snap
tests/snap/hover/expression_context/03_pass_semantics.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Implicit conversions**

Argument hover reports the target type of an implicit conversion

When an argument reaches a parameter through an implicit conversion, the
card notes the target type, for both built-in and user-defined
conversions.

```snap
tests/snap/hover/expression_context/04_implicit_conversion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1016 -->

**String literals**

String-literal hover reports its size in bytes

A string-literal card reports the array type and its size in bytes
(`const char[6]`, `Size: 6 bytes` — the length plus the null
terminator), not an explicit character count.

```snap
tests/snap/hover/expression_context/05_string_length.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1669 -->

**Numeric literals**

Numeric literals have no type or value hover yet

Hovering a numeric literal yields no card, unlike character and string
literals, whose type and value are shown.

```snap
tests/snap/hover/expression_context/06_numeric_literal_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1622 -->

**Record variables**

Record hover can show a misleading enclosing constant value

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

Declaration documentation appears on hover

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

Copied documentation is not resolved onto the receiving symbol yet

A `@copydoc target` tag remains literal instead of copying `target`'s
documentation into this symbol's hover card.

```snap
tests/snap/hover/documentation/03_copydoc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2504 -->

**Inherited override docs**

An override with no comment does not inherit the base method's documentation
yet

An overriding method with no comment of its own has no description on its
hover card.

```snap
tests/snap/hover/documentation/04_inherit_overridden_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2506 -->

**Overload doc sharing**

A later overload does not reuse the first overload's documentation yet

Consecutive overloads often document only the first; a later undocumented
overload has no description on its hover card.

```snap
tests/snap/hover/documentation/05_overload_doc_sharing.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1936 -->

**Inherited constructor docs**

Inherited constructors have no documentation hover yet

The name in a `using Base::Base;` declaration resolves to the class rather
than an inherited constructor, so the base constructor's documentation has
no hover surface.

```snap
tests/snap/hover/documentation/06_inherited_ctor_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#974 -->

**Banner comments**

A separated section banner still attaches to the following declaration

A `// ==== Section ====` banner followed by a blank line is misattributed
as documentation for the declaration below it, so the banner text appears
in the card.

```snap
tests/snap/hover/documentation/07_comment_association.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration vs definition comments**

The declaration's doc wins over a definition-site comment

The declaration's `///` documentation appears at both the declaration and
definition sites instead of the definition's plain `//` note.

```snap
tests/snap/hover/documentation/08_decl_vs_def_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2057 -->

**Whitespace and newlines**

Markdown tables in comments lose their line breaks on hover

A markdown table written across several `///` lines is flattened onto one
line, so the table does not render.

```snap
tests/snap/hover/documentation/09_whitespace_preserve.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1040 -->

**Comment indentation**

Indented documentation blocks lose their leading indentation on hover

A doc comment whose body contains an indented block loses the block's
offset, and its blank line collapses.

```snap
tests/snap/hover/documentation/10_comment_indentation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1226 -->

**Template keyword from a macro**

Documentation on a macro-produced template is missing from hover

When the `template` keyword is produced by a macro expansion, the
declaration's doc comment does not appear on hover, and the card carries
no description.

```snap
tests/snap/hover/documentation/11_macro_template_doc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2148 -->

**Comment suppression option**

Misattributed documentation cannot be suppressed by configuration yet

A stray comment such as a section banner separated from the code by a blank
line always reaches the hover card, and no option suppresses it.

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

A nested macro argument shows its definition without an expansion preview

The expansion preview starts at the outer invocation, so hovering an
inner macro named inside the arguments shows only its definition, not an
expansion preview.

```snap
tests/snap/hover/macro_hover/04_nested_arg_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2642 -->

**Use before definition**

A macro use before its definition has no hover yet

A macro name used in an `#if` above its own `#define` has no hover, while a
use after the definition works normally.

```snap
tests/snap/hover/macro_hover/05_expansion_before_definition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`#define` inside the preamble**

A leading macro definition has no hover card

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

Language keywords do not have documentation hover yet

Hovering a keyword such as `const` or `virtual` produces no card.

```snap
tests/snap/hover/special_hover_targets/03_keyword_docs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1862 -->

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

GObject Introspection annotations do not appear in hover cards yet

GTK-Doc / kernel-doc comment syntax and GObject Introspection
annotations are not parsed into the hover card.

```snap
tests/snap/hover/special_hover_targets/09_gtk_doc.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2669 -->

**LaTeX math in Doxygen**

Inline Doxygen formulas are not rendered as math

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

Cards render as markdown, or plain text via `parse_comment_as_markdown =
false`

```snap
tests/snap/hover/presentation/01_presentation.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module-Related

<!-- BEGIN GENERATED ITEMS: module_related -->

<!-- BEGIN CAPABILITY: unsupported -->

**Import statement hover**

Hovering `import` does not describe the imported module yet

Hovering an `import` declaration does not yet describe the imported
module.

```snap
tests/snap/hover/module_related/01_import_hover.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module name hover**

Hovering a module name does not list its owning files yet

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

MSVC inheritance model attributes do not alter record hover

Under an MSVC target, the implicit inheritance attribute does not leak
into the record or method card.

```snap
tests/snap/hover/hover_correctness/01_ms_inheritance.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Most-vexing-parse**

Direct initialization and a function declaration have distinct hover cards

The direct initialization appears as a variable, while the most-vexing
form appears as a function declaration.

```snap
tests/snap/hover/hover_correctness/02_object_vs_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Large unsigned enum constant**

Hovering a `0xFFFF...ULL` enumerator does not crash

The card renders the full unsigned value without overflowing or failing.

```snap
tests/snap/hover/hover_correctness/03_large_enum_value.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call with default arguments**

Hovering a call that omits defaults does not crash

The call card renders the callee signature with its default arguments.

```snap
tests/snap/hover/hover_correctness/04_default_args_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-shadowed symbol**

A function-like macro shadows a same-named function at the call site

The card shows the active macro and its expansion instead of the shadowed
function.

```snap
tests/snap/hover/hover_correctness/05_macro_shadowed_symbol.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
