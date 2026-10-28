# Document Symbols

<!-- The cards in the sections below are generated from the documented cases
     in tests/document_symbols/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`: a nested symbol tree with ranges, selection ranges and a `detail` field that disambiguates overloads and shows declared types.

## Symbol Hierarchy

Symbols nest in the outline as they nest in the code: a member under its class, a local under its function.

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Nested scopes**

Namespaces, classes and their members nest in the outline as they nest in the code

```case
tests/document_symbols/symbol_hierarchy/01_nested_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Out-of-line definitions**

An out-of-line definition lists where it is written, under its qualified name, and a reopened namespace lists once per opening

```case
tests/document_symbols/symbol_hierarchy/02_qualified_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous and inline scopes**

Anonymous namespaces, structs and unions group their members under a placeholder name, and an inline namespace keeps its own node

```case
tests/document_symbols/symbol_hierarchy/03_anonymous_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**Local symbols**

Variables, types and lambdas declared in a function body nest under the function

```case
tests/document_symbols/symbol_hierarchy/04_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend function definitions**

A friend function defined inside a class lists under that class

```case
tests/document_symbols/symbol_hierarchy/05_friend_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Access sections**

Members list in the order they are written; their `public` and `private` sections are no level of the outline

```case
tests/document_symbols/symbol_hierarchy/06_access_sections
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#500 -->

**Members a macro declares**

What one macro invocation declares under names written nowhere lists under the invocation, with what it names in its arguments: a test macro's test, the members a class macro adds; an invocation that declares only names its arguments write lists them as written

```case
tests/document_symbols/symbol_hierarchy/09_macro_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Kinds

Each symbol lists with its kind, from namespaces and classes to macros and module declarations.

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Core symbol kinds**

Namespaces, classes, unions, enums and their members, functions, variables, fields, bindings and lambdas each list with their kind

```case
tests/document_symbols/symbol_kinds/01_core_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Templates**

Class, function and variable templates read `template` in their detail, and their extent starts at the `template` keyword; concepts and abbreviated templates list too

```case
tests/document_symbols/symbol_kinds/02_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Specializations and deduction guides**

Specializations list with their template arguments and their own members, and a deduction guide with the signature it deduces

```case
tests/document_symbols/symbol_kinds/03_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

`typedef` and `using` aliases and alias templates list as type aliases

```case
tests/document_symbols/symbol_kinds/04_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Explicit instantiations**

Explicit instantiations of class, function and variable templates list without members

```case
tests/document_symbols/symbol_kinds/05_explicit_instantiations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**Macros**

Object-like and function-like macros list, a function-like one with its parameters as detail

```case
tests/document_symbols/symbol_kinds/06_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module declarations**

A module unit lists the module it declares or belongs to

```case
tests/document_symbols/symbol_kinds/07_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include directives**

`#include` directives do not list in the outline; their document links lead to the files they name

```case
tests/document_symbols/symbol_kinds/10_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pragma mark sections**

`#pragma mark - Name` opens a section that groups the symbols after it

```case
tests/document_symbols/symbol_kinds/11_pragma_marks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespace aliases**

A namespace alias lists as a namespace, `namespace alias` its detail

```case
tests/document_symbols/symbol_kinds/20_namespace_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include guards**

An include guard's macro does not list: the `#ifndef` and `#define` that wrap the whole header

```case
tests/document_symbols/symbol_kinds/21_include_guards
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Detail

The `detail` beside a name tells overloads apart and shows a variable's declared type.

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**Function signatures**

The `detail` field shows a function's parameters, names included, and its return type, which tells overloads apart; constructors show no return type

```case
tests/document_symbols/symbol_detail/01_detail_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable and field types**

A variable or field shows its declared type as detail, a lambda what calling it takes and returns

```case
tests/document_symbols/symbol_detail/02_variable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default arguments**

A function's detail lists its parameters with their default values

```case
tests/document_symbols/symbol_detail/03_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#601 -->

**Base classes**

A derived class's detail names its base classes as written, with their access

```case
tests/document_symbols/symbol_detail/04_base_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scoped types**

A type written with its class scope keeps that scope once in the detail, dependent names included

```case
tests/document_symbols/symbol_detail/05_scoped_types
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Ranges

Each symbol carries the extent of its whole declaration and the range of its name, which the editor selects when you pick it.

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**Names and extents**

A symbol's extent covers its whole declaration, and its name the whole written name, `~Widget`, `operator==` and `operator bool` included

```case
tests/document_symbols/symbol_ranges/01_selection_ranges
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**Multiline signatures**

A declaration spanning lines extends from its first line to its last

```case
tests/document_symbols/symbol_ranges/02_multiline_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**Symbols from macros**

A symbol a macro invocation declares lists at the invocation, not at the macro's definition

```case
tests/document_symbols/symbol_ranges/03_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**Names in macro arguments**

A name written in a macro's arguments is the symbol's name; one written in the macro's body falls back to the invocation

```case
tests/document_symbols/symbol_ranges/04_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Tags

Tags mark a symbol as deprecated; what else a member is, its detail says.

<!-- BEGIN GENERATED ITEMS: document_symbols/symbol_tags -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated symbols**

A deprecated symbol carries the Deprecated tag, which editors strike through

```case
tests/document_symbols/symbol_tags/01_deprecated
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Member markers**

A member's detail says `static`, `virtual` and `= 0` where the member is so; the protocol's tags carry only deprecation, and access is not shown

```case
tests/document_symbols/symbol_tags/02_modifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
