# Document Symbols

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_symbol/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

Provides the file outline and breadcrumb navigation via `textDocument/documentSymbol`: a nested symbol tree with ranges, selection ranges and a `detail` field that disambiguates overloads and shows declared types.

## Symbol Hierarchy

<!-- BEGIN GENERATED ITEMS: symbol_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Nested symbol tree**

Symbols nest by their written scope; out-of-line definitions appear at their lexical position with qualified names

```snap
tests/snap/document_symbol/symbol_hierarchy/01_hierarchy_nesting.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Symbol ranges and selection ranges**

The range spans the whole declaration; the selection range covers the full written name, including multi-token names like `~Widget`, `operator==` and `operator bool`

```snap
tests/snap/document_symbol/symbol_hierarchy/02_hierarchy_selection_ranges.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#499 -->

**Access specifier grouping**

`public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation

```snap
tests/snap/document_symbol/symbol_hierarchy/03_hierarchy_access_specifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous and inline scopes**

Anonymous namespaces, unnamed structs and unions group their members under a placeholder name; inline namespace members stay under the inline namespace node

```snap
tests/snap/document_symbol/symbol_hierarchy/04_hierarchy_anonymous.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**UTF-16 position encoding**

Columns after non-ASCII text count UTF-16 code units

```snap
tests/snap/document_symbol/symbol_hierarchy/05_hierarchy_utf16.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Kinds

<!-- BEGIN GENERATED ITEMS: symbol_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Core symbol kinds**

namespaces, classes, structs, unions, enums and their members, functions, variables, fields, structured bindings and lambdas all appear in the outline with a mapped LSP symbol kind

```snap
tests/snap/document_symbol/symbol_kinds/01_kinds_basic.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template declarations**

class, function and variable templates carry a `template ` detail prefix; concepts and abbreviated function templates (`concept auto` parameters) appear as well

```snap
tests/snap/document_symbol/symbol_kinds/02_kinds_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template specializations and deduction guides**

Explicit and partial specializations of class and variable templates appear with their template arguments in the name; members nest under their specialization; deduction guides render their deduced signature

```snap
tests/snap/document_symbol/symbol_kinds/03_kinds_specializations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

`typedef`, `using` aliases and alias templates appear in the outline with a `type alias` detail

```snap
tests/snap/document_symbol/symbol_kinds/04_kinds_type_aliases.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Explicit instantiation directives**

The class forms appear as childless symbols; clang mislocates the function and variable forms at the pattern, so they are missing from the outline

```snap
tests/snap/document_symbol/symbol_kinds/05_kinds_explicit_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1744 -->

**Macro definitions**

object-like and function-like macro definitions in the outline, a parameter list as the function-like detail

```snap
tests/snap/document_symbol/symbol_kinds/06_kinds_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Macros in the preamble region**

Definitions in the leading directive run outline on the inspect path, while the server's preamble record does not surface them yet

```snap
tests/snap/document_symbol/symbol_kinds/07_macro_preamble.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Detail

<!-- BEGIN GENERATED ITEMS: symbol_detail -->

<!-- BEGIN CAPABILITY: supported clangd#520 clangd#601 clangd#1232 -->

**Function signatures**

Parameter and return types in the `detail` field disambiguate overloads; constructors drop the `void` return type

```snap
tests/snap/document_symbol/symbol_detail/01_detail_signatures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable and field types**

The declared type in the `detail` field; lambdas render as `(lambda)`

```snap
tests/snap/document_symbol/symbol_detail/02_detail_variable_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#221 -->

**Default argument stripping**

The signature is derived from the function type, so default parameter values never leak into the outline

```snap
tests/snap/document_symbol/symbol_detail/03_detail_default_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Base classes in detail**

Show `: Shape` on derived class declarations

```snap
tests/snap/document_symbol/symbol_detail/04_detail_base_classes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2221 -->

**Multiline signature ranges**

The symbol range starts at the beginning of the declaration and spans the full signature, so editor sticky scroll anchors correctly

```snap
tests/snap/document_symbol/symbol_detail/05_detail_multiline_signatures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scoped types**

A written class scope appears in the detail exactly once, for nested classes, template-ids, aliases and dependent names alike

```snap
tests/snap/document_symbol/symbol_detail/06_detail_scoped_types.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Missing Symbols

<!-- BEGIN GENERATED ITEMS: missing_symbols -->

<!-- BEGIN CAPABILITY: unsupported clangd#2226 -->

**Include directives**

`#include` entries in the outline

```snap
tests/snap/document_symbol/missing_symbols/01_missing_includes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#616 -->

**Local symbols**

Variables and types declared inside function bodies nest under their function

```snap
tests/snap/document_symbol/missing_symbols/02_local_symbols.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module declarations**

`export module`, `module` and `import` declarations in the outline

```snap
tests/snap/document_symbol/missing_symbols/03_missing_modules.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`#pragma mark` navigation markers**

Editor section markers as outline entries

```snap
tests/snap/document_symbol/missing_symbols/04_missing_pragma_mark.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend function definitions**

A friend function defined inline in a class appears under that class

```snap
tests/snap/document_symbol/missing_symbols/05_friend_definitions.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Symbol Tags

<!-- BEGIN GENERATED ITEMS: symbol_tags -->

<!-- BEGIN CAPABILITY: unsupported -->

**Deprecated tag**

Mark `[[deprecated]]` symbols with the LSP `deprecated` symbol tag

```snap
tests/snap/document_symbol/symbol_tags/01_tags_deprecated.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2123 -->

**Access and storage indicators**

Public / private / protected, static, virtual and abstract markers on outline entries

```snap
tests/snap/document_symbol/symbol_tags/02_tags_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Location Correctness

<!-- BEGIN GENERATED ITEMS: location_correctness -->

<!-- BEGIN CAPABILITY: supported clangd#475 -->

**Symbols from macro expansions**

A symbol produced by a macro invocation is located at the invocation, not at the macro definition

```snap
tests/snap/document_symbol/location_correctness/01_macro_symbols.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1941 -->

**Names spelled in macro arguments**

The selection range points at the name written in the macro argument; names spelled in the macro body fall back to the invocation site

```snap
tests/snap/document_symbol/location_correctness/02_macro_argument_names.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
