# Hover

Rich information cards for the symbol under the cursor.

<!-- The cards in the sections below are generated from the documented cases
     in tests/hover/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Symbols

The card names what the symbol is and the scope it is declared in, and shows its declaration.

<!-- BEGIN GENERATED ITEMS: hover/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**Symbol kinds**

The card's title names what the symbol is: namespace, struct, union, enum, enumerator, alias, field, function

```case
tests/hover/symbols/01_symbol_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scopes**

The card says which namespace, class or function the symbol is declared in

```case
tests/hover/symbols/02_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Access**

A member's definition reads its `public`, `protected` or `private` access

```case
tests/hover/symbols/03_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definitions**

The card shows the symbol's declaration as source, at its definition and at its uses

```case
tests/hover/symbols/04_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2474 -->

**Virtual methods**

A method's definition reads `virtual`, `override` or `final` as written, so an override written without them gives no sign it is virtual

```case
tests/hover/symbols/05_virtual_modifiers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#436 -->

**Anonymous namespaces**

The scope of a card names the anonymous namespace a name is declared in

```case
tests/hover/symbols/06_anonymous_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated symbols**

A deprecated function's card says so, with the message its deprecation gives

The message of `[[deprecated]]` usually tells what to use instead.

```case
tests/hover/symbols/13_deprecation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Symbols from headers**

A symbol declared in an included header shows its documentation and definition

```case
tests/hover/symbols/14_header_symbols
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Functions

A function's card lists its return type and its parameters.

<!-- BEGIN GENERATED ITEMS: hover/functions -->

<!-- BEGIN CAPABILITY: supported -->

**Function signatures**

A function's card lists its return type and each parameter with its name and default

```case
tests/hover/functions/01_signatures
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Variables

A variable's card spells its type as written, and its value when that is known at compile time.

<!-- BEGIN GENERATED ITEMS: hover/variables -->

<!-- BEGIN CAPABILITY: supported -->

**Declarator types**

A variable's card spells its pointer, reference and array declarators as written

```case
tests/hover/variables/01_declarators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constant values**

A variable or enumerator whose value is known at compile time shows it

```case
tests/hover/variables/02_constant_values
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#710 -->

**Long initializers**

A long initializer is left out of the definition, and the value shows its first elements

```case
tests/hover/variables/04_long_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__func__`**

Hovering `__func__` shows the name of the function it is in, when the function is not a template

```case
tests/hover/variables/06_predefined_identifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Members of Types

A class lists its members and an enum its enumerators.

<!-- BEGIN GENERATED ITEMS: hover/members -->

<!-- BEGIN CAPABILITY: supported clangd#959 -->

**Members of types**

A class lists its data members and member types, an enum its enumerators with their values

`max_members` caps the list, 20 by default; `0` shows the declaration alone.

```case
tests/hover/members/01_type_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Aliases

A type spelled through an alias shows what the alias stands for.

<!-- BEGIN GENERATED ITEMS: hover/aliases -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

A type spelled through an alias shows what it stands for, as `Alias (aka int)`

With `show_aka = false` the card keeps the alias alone.

```case
tests/hover/aliases/01_aka
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2020 -->

**Aliases of structs**

An alias names the struct it stands for, but does not show its members yet

```case
tests/hover/aliases/02_alias_targets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2219 -->

**C anonymous struct typedefs**

In C, a struct named only by its typedef shows under the typedef's name

The definition line still spells it `struct Point`, a name C does not
know for it.

```case
tests/hover/aliases/03_c_anonymous_structs
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Deduced Types

`auto` and `decltype` show the type they stand for.

<!-- BEGIN GENERATED ITEMS: hover/deduced_types -->

<!-- BEGIN CAPABILITY: supported -->

**`auto`**

Hovering `auto` shows the type it stands for, in a template as the template would write it

A type only instantiation can work out, such as that of `value + 1`, is not deduced yet.

```case
tests/hover/deduced_types/01_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype`**

Hovering `decltype` shows the type it yields, the reference a parenthesized name adds included

```case
tests/hover/deduced_types/02_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**auto through an alias**

An `auto` deduced from an alias keeps the alias's name beside what it stands for

```case
tests/hover/deduced_types/03_alias_spelling
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#493 clangd#1768 -->

**Generic lambda parameters**

A generic lambda's `auto` parameter is the type the lambda is called with, and the parameter's members have cards

Inside the body the parameter reads as the type of the call, and `point.x`
is the member of that type. A lambda called with several types keeps its
parameter `auto`.

```case
tests/hover/deduced_types/06_generic_lambdas
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Templates

A template parameter shows its kind and default, and a use of a template the arguments it is instantiated with.

<!-- BEGIN GENERATED ITEMS: hover/templates -->

<!-- BEGIN CAPABILITY: supported -->

**Template parameters**

A template parameter shows its kind, a type, a template or a value, with its default

```case
tests/hover/templates/01_template_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#230 -->

**Template arguments**

A use of a template shows the types it is instantiated with, but not which parameter each argument binds yet

```case
tests/hover/templates/02_instantiation_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#435 -->

**Deduced class arguments**

A variable whose class template arguments are deduced shows them, while the class name beside it shows the template alone

```case
tests/hover/templates/03_deduced_class_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Concept constraints**

A concept and a constrained parameter show the constraint, but an `Addable auto` placeholder shows only the deduced type

```case
tests/hover/templates/04_concept_constraints
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Layout

Fields and classes show their size and alignment as the compiler lays them out.

<!-- BEGIN GENERATED ITEMS: hover/layout -->

<!-- BEGIN CAPABILITY: supported -->

**Field layout**

A field shows its offset, size and alignment, and the padding after it

```case
tests/hover/layout/01_field_layout
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1763 -->

**Type layout**

A class shows its size and alignment, but not how much of it is padding yet

```case
tests/hover/layout/02_type_layout
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1771 -->

**Vtable slots**

A virtual method does not show its slot in the vtable yet

```case
tests/hover/layout/03_vtable_slots
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Call Arguments

An argument at a call shows the parameter it is passed to, and how it is passed.

<!-- BEGIN GENERATED ITEMS: hover/arguments -->

<!-- BEGIN CAPABILITY: supported -->

**Call arguments**

An argument at a call shows the parameter it is passed as

```case
tests/hover/arguments/01_bound_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pass modes**

An argument's card says whether it is passed by value, by reference or by const reference

```case
tests/hover/arguments/02_pass_modes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Implicit conversions**

An argument converted on its way to the parameter names the type it is converted to

```case
tests/hover/arguments/03_conversions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1622 -->

**Record arguments**

A record passed to a call that is evaluated at compile time shows the call's value as its own

```case
tests/hover/arguments/04_record_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Expressions

Literals and `this` show what they are.

<!-- BEGIN GENERATED ITEMS: hover/expressions -->

<!-- BEGIN CAPABILITY: unsupported clangd#1016 clangd#1669 -->

**Literals**

A string literal shows its type and size, while a number shows nothing yet

The size counts the terminating null; the string's length is not shown.

```case
tests/hover/expressions/01_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this`**

Hovering `this` shows the type it points to, in a class and in a class template

```case
tests/hover/expressions/02_this
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

A macro shows its `#define`, and a use of it what it expands to.

<!-- BEGIN GENERATED ITEMS: hover/macros -->

<!-- BEGIN CAPABILITY: partial clangd#2642 -->

**Use before definition**

A macro name used above its own `#define` has no card, while a use below it does

```case
tests/hover/macros/01_use_before_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Every site of a macro**

The definition, a use, `#ifdef`, `defined` and `#undef` all show the macro's `#define`

```case
tests/hover/macros/02_definition_sites
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expansion preview**

A use of a function-like macro shows what it expands to with its arguments

```case
tests/hover/macros/03_expansion_preview
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Command-line macros**

A macro defined with `-D` shows the `#define` it amounts to

```case
tests/hover/macros/04_command_line
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macros in arguments**

A macro inside another macro's arguments shows its definition and what it expands to there

```case
tests/hover/macros/05_nested_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Include Directives

An `#include` shows the file it resolves to.

<!-- BEGIN GENERATED ITEMS: hover/directives -->

<!-- BEGIN CAPABILITY: supported -->

**Include directives**

Hovering an `#include` shows the file it resolves to

```case
tests/hover/directives/01_include_paths
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Documentation

A declaration's documentation comment shows in its card, wherever the symbol is hovered. The comment is read the way Doxygen reads it — its commands, and the Markdown it is written in — and `clice query` reads it the same way.

<!-- BEGIN GENERATED ITEMS: hover/documentation -->

<!-- BEGIN CAPABILITY: supported -->

**Documentation comments**

A declaration's `///` comment shows in its card, at the declaration and at every use

A use of a template shows the comment of the specialization it picks.

```case
tests/hover/documentation/01_doc_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Getters and setters**

A trivial getter or setter without a comment of its own is described as accessing its field

```case
tests/hover/documentation/02_accessors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1320 -->

**@copydoc**

A `@copydoc` command shows the documentation of what it names

```case
tests/hover/documentation/03_copydoc
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2504 -->

**Overrides**

An override without a comment of its own shows the documentation of the method it overrides

```case
tests/hover/documentation/04_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2506 -->

**Overloads**

An overload without a comment of its own shows none, whatever its neighbours say

Different overloads often do different things, so an undocumented one does not
borrow the comment of the overload before it.

```case
tests/hover/documentation/05_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1936 -->

**Inherited constructors**

The name in `using Base::Base` shows the constructor it inherits, with its documentation

```case
tests/hover/documentation/06_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#974 -->

**Banner comments**

A section banner a blank line separates from a declaration is not its documentation

```case
tests/hover/documentation/07_banner_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration and definition**

The declaration's `///` comment shows at the declaration and at the definition

The definition's plain `//` comment does not take its place, whichever of
the two is hovered first.

```case
tests/hover/documentation/08_declaration_and_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2057 -->

**Markdown tables**

A table in a comment shows as a table

```case
tests/hover/documentation/09_markdown_tables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1040 -->

**Indented blocks**

An indented block in a comment keeps its indentation and the blank line before it

```case
tests/hover/documentation/10_indented_blocks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2148 -->

**Stray comments**

A plain comment a blank line separates from a declaration is not its documentation

A `///` or `/** */` comment documents the declaration below it even across
a blank line, as Doxygen reads it.

```case
tests/hover/documentation/11_stray_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2662 -->

**GTK-Doc comments**

GTK-Doc's name line goes, and each `@parameter:` entry shows beside its parameter

```case
tests/hover/documentation/12_gtk_doc
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2669 -->

**Formulas**

A Doxygen formula shows as code, without its `@f$` markers

```case
tests/hover/documentation/13_latex_formulas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Doxygen commands**

`@brief` heads the card, and each `@param` and `@return` shows beside what it describes

```case
tests/hover/documentation/18_doxygen_commands
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter documentation**

A parameter's card shows what its function's `@param` or its template's `@tparam` says of it

```case
tests/hover/documentation/20_parameter_docs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#782 -->

**Macro documentation**

A macro's documentation comment shows in its card

```case
tests/hover/documentation/21_macro_docs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Trailing comments**

A `///<` comment after a field, an enumerator or a variable on its line documents it; any other comment there does when no comment above documents it

```case
tests/hover/documentation/22_trailing_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Comments as written**

The plain format shows a comment as written, its commands and markup included

```case
tests/hover/documentation/23_plain_format
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**System headers**

A declaration in a system header shows its documentation comment too

```case
tests/hover/documentation/25_system_headers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Modules

A module name shows the file of its interface unit, and a declaration from a module shows whether it is exported and the documentation its interface gives it.

<!-- BEGIN GENERATED ITEMS: hover/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module names**

A module name shows the file of its interface unit, in an `import` and in the module declaration

```case
tests/hover/modules/01_module_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Exported declarations**

A declaration of a module interface reads `export` when the module exports it

```case
tests/hover/modules/02_export_status
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Documented exports**

A symbol imported from a module shows the documentation written in its interface

```case
tests/hover/modules/04_documented_exports
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Attributes and Keywords

Attributes and keywords such as `const` or `virtual` have meanings of their own for a card to explain.

<!-- BEGIN GENERATED ITEMS: hover/attributes -->

<!-- BEGIN CAPABILITY: supported clangd#1862 -->

**Attributes**

Hovering an attribute shows clang's description of it, for GNU and standard spellings

```case
tests/hover/attributes/01_attributes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1862 -->

**Keywords**

Hovering a keyword such as `const` or `virtual` shows nothing yet

```case
tests/hover/attributes/02_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Nothing to Show

Where there is nothing worth a card, none appears.

<!-- BEGIN GENERATED ITEMS: hover/no_card -->

<!-- BEGIN CAPABILITY: supported -->

**Nothing to show**

A builtin type keyword or the inside of an empty body shows no card

```case
tests/hover/no_card/01_meaningless_tokens
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Presentation

A card is markdown, or plain text for an editor that cannot show markdown.

<!-- BEGIN GENERATED ITEMS: hover/presentation -->

<!-- BEGIN CAPABILITY: supported -->

**Plain text cards**

Cards are markdown, or plain text for editors that cannot show markdown

An editor says which it can show when it starts, and the card follows.

```case
tests/hover/presentation/01_plain_text
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
