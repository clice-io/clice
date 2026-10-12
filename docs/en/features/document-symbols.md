# Document Symbols

<!-- The cards in the sections below are generated from the documented cases
     in tests/document_symbol/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`: a nested symbol tree with ranges, selection ranges and a `detail` field that disambiguates overloads and shows declared types.

## Symbol Hierarchy

Symbols nest in the outline as they nest in the code: a member under its class, a local under its function.

<!-- BEGIN GENERATED ITEMS: document_symbol/symbol_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Nested scopes**

Namespaces, classes and their members nest in the outline as they nest in the code

```case
tests/document_symbol/symbol_hierarchy/01_nested_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Out-of-line definitions**

An out-of-line definition lists where it is written, under its qualified name, and a reopened namespace lists once per opening

```case
tests/document_symbol/symbol_hierarchy/02_qualified_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous and inline scopes**

Anonymous namespaces, structs and unions group their members under a placeholder name, and an inline namespace keeps its own node

```case
tests/document_symbol/symbol_hierarchy/03_anonymous_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**Local symbols**

Variables, types and lambdas declared in a function body nest under the function

```case
tests/document_symbol/symbol_hierarchy/04_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend function definitions**

A friend function defined inside a class lists under that class

```case
tests/document_symbol/symbol_hierarchy/05_friend_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#499 -->

**Access sections**

Members are not grouped under their `public` and `private` sections yet

```case
tests/document_symbol/symbol_hierarchy/06_access_sections
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Kinds

Each symbol lists with its kind, from namespaces and classes to macros and module declarations.

<!-- BEGIN GENERATED ITEMS: document_symbol/symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Core symbol kinds**

Namespaces, classes, unions, enums and their members, functions, variables, fields, bindings and lambdas each list with their kind

```case
tests/document_symbol/symbol_kinds/01_core_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Templates**

Class, function and variable templates read `template` in their detail, and concepts and abbreviated templates list too

A primary template's extent starts after its `template <...>` line, where
a specialization's includes it.

```case
tests/document_symbol/symbol_kinds/02_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Specializations and deduction guides**

Specializations list with their template arguments and their own members, and a deduction guide with the signature it deduces

```case
tests/document_symbol/symbol_kinds/03_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Type aliases**

`typedef` and `using` aliases and alias templates list as type aliases

```case
tests/document_symbol/symbol_kinds/04_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Explicit instantiations**

Explicit instantiations of class, function and variable templates list without members

```case
tests/document_symbol/symbol_kinds/05_explicit_instantiations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**Macros**

Object-like and function-like macros list, a function-like one with its parameters as detail

Macros in the leading run of directives, before any declaration, are
missing from the editor's outline yet.

```case
tests/document_symbol/symbol_kinds/06_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module declarations**

A module interface unit lists the module it declares

```case
tests/document_symbol/symbol_kinds/07_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2226 -->

**Include directives**

`#include` directives do not list in the outline yet

```case
tests/document_symbol/symbol_kinds/10_includes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pragma mark sections**

`#pragma mark - Name` opens a section that groups the symbols after it

```case
tests/document_symbol/symbol_kinds/11_pragma_marks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Detail

The `detail` beside a name tells overloads apart and shows a variable's declared type.

<!-- BEGIN GENERATED ITEMS: document_symbol/symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**Function signatures**

Parameter and return types in the `detail` field disambiguate overloads; constructors drop the `void` return type

```case
tests/document_symbol/symbol_detail/01_detail_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable and field types**

A variable or field shows its declared type as detail, a lambda `(lambda)`

```case
tests/document_symbol/symbol_detail/02_variable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#221 -->

**Default arguments**

A function's detail lists its parameter types without their default values

```case
tests/document_symbol/symbol_detail/03_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Base classes**

A derived class's detail does not name its base classes yet

```case
tests/document_symbol/symbol_detail/04_base_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Scoped types**

A type written with its class scope keeps that scope once in the detail, dependent names included

```case
tests/document_symbol/symbol_detail/05_scoped_types
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Ranges

Each symbol carries the extent of its whole declaration and the range of its name, which the editor selects when you pick it.

<!-- BEGIN GENERATED ITEMS: document_symbol/symbol_ranges -->

<!-- BEGIN CAPABILITY: supported -->

**Names and extents**

A symbol's extent covers its whole declaration, and its name the whole written name, `~Widget`, `operator==` and `operator bool` included

```case
tests/document_symbol/symbol_ranges/01_selection_ranges
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**Multiline signatures**

A declaration spanning lines extends from its first line to its last

```case
tests/document_symbol/symbol_ranges/02_multiline_signatures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**Symbols from macros**

A symbol a macro invocation declares lists at the invocation, not at the macro's definition

```case
tests/document_symbol/symbol_ranges/03_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**Names in macro arguments**

A name written in a macro's arguments is the symbol's name; one written in the macro's body falls back to the invocation

```case
tests/document_symbol/symbol_ranges/04_macro_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Tags

Tags mark a symbol as deprecated or describe its access and storage.

<!-- BEGIN GENERATED ITEMS: document_symbol/symbol_tags -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated symbols**

A deprecated symbol carries the Deprecated tag, which editors strike through

```case
tests/document_symbol/symbol_tags/01_deprecated
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2123 -->

**Access and storage tags**

The outline does not tag members with their access, `static`, `virtual` or abstractness yet

```case
tests/document_symbol/symbol_tags/02_modifiers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
