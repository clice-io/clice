# Code Completion

Candidates for what is being typed: names, members, calls, headers and modules.

<!-- The cards in the sections below are generated from the documented cases
     in tests/completion/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Symbols

Names in scope complete as you type, and a qualifier such as `ns::` lists what that scope declares.

<!-- BEGIN GENERATED ITEMS: completion/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**Fuzzy unqualified lookup**

Strong prefix matches survive, while weak subsequence matches do not

```case
tests/completion/symbols/01_unqualified_lookup
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class template deduplication**

A class template with constructors and a deduction guide stays a single entry

```case
tests/completion/symbols/02_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macros**

Object-like macros complete as constants, function-like ones as functions with a parameter signature

Their arguments follow the functions' setting, `code_completion.arguments`.

```case
tests/completion/symbols/05_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespace-qualified lookup**

`ns::` lists the namespace's own members

```case
tests/completion/symbols/07_namespace_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum members**

A scoped enum lists its enumerators through `Type::`, an unscoped enumerator completes by its bare name

```case
tests/completion/symbols/08_enum_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Required qualifier**

An enumerator the bare name does not reach completes with the qualifier it needs, matched against its bare name

```case
tests/completion/symbols/11_required_qualifier
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Documentation**

An item shows the documentation comment of what it names, a member of a template's instantiation the one its template wrote

```case
tests/completion/symbols/13_documentation
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Namespaces

A name another namespace declares completes too, with the qualifier that reaches it; one the file does not include or import yet brings the `#include` or the `import` it needs.

<!-- BEGIN GENERATED ITEMS: completion/scopes -->

<!-- BEGIN CAPABILITY: supported -->

**Names of other namespaces**

A name another namespace declares completes with the qualifier that reaches it, after the names in scope

```case
tests/completion/scopes/01_other_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Headers not included yet**

A name the project declares in a header the file does not include yet completes with the `#include` that brings it

```case
tests/completion/scopes/02_include_insertion
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Modules not imported yet**

A name a module of the project exports completes with the `import` that brings it, its own partitions by their partition name

```case
tests/completion/scopes/03_import_insertion
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Member Access

After `.`, `->` or `::`, the members of the object or the scope complete.

<!-- BEGIN GENERATED ITEMS: completion/member_access -->

<!-- BEGIN CAPABILITY: supported -->

**Members after a dot**

Completing after a dot lists the members of the object's class

```case
tests/completion/member_access/01_struct_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scope-qualified members**

After `Class::` static data, nested types, methods and the injected class name all list

Qualified completion is not filtered to what a static context can use: instance fields and the destructor show up alongside the static members and nested types.

```case
tests/completion/member_access/04_scope_access
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inherited members**

A derived object completes its own members and those of its base

```case
tests/completion/member_access/05_inherited_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Designated initializer fields**

`{ .` lists the fields of the aggregate being initialized, those of a matching partial specialization for a dependent one

```case
tests/completion/member_access/06_designated_initializer
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inaccessible members**

Private and protected members are not offered where they cannot be used

```case
tests/completion/member_access/07_inaccessible_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Out-of-line definitions**

After `Class::` in a definition, every member is offered, private ones included

```case
tests/completion/member_access/10_out_of_line_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dot on a pointer**

After `.` on a pointer, the members `->` reaches complete and turn the `.` into `->`

```case
tests/completion/member_access/17_dot_on_pointer
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Functions and Snippets

A function completes as a call, with what `code_completion.arguments` asks for between its parentheses. All options below live in the `[code_completion]` configuration section.

<!-- BEGIN GENERATED ITEMS: completion/functions_snippets -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter placeholder snippets**

Calls insert tab-stop placeholders per argument; a no-argument function gets its empty parentheses

With `delimiters`, a call gets its parentheses with the cursor between them; with `none`, the name alone.

```case
tests/completion/functions_snippets/01_snippet_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Signature and return type**

The parameter list and the return type ride along with a function's name

```case
tests/completion/functions_snippets/02_function_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overload bundling**

An overload set collapses into one entry with an overload count

With bundling off, every overload is its own entry with its own signature.

```case
tests/completion/functions_snippets/03_overload_bundle
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call parentheses**

A completed call gets its parentheses, unless arguments already follow the name or it is not being called

```case
tests/completion/functions_snippets/06_call_parentheses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template argument placeholders**

A class template inserts a placeholder per template parameter without a default, and empty brackets when every parameter has one

```case
tests/completion/functions_snippets/07_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Statement keywords**

Statement keywords complete as keywords; the option inserts the whole statement with a placeholder for each part

```case
tests/completion/functions_snippets/08_statement_snippets
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Templates and Concepts

Inside a template, members complete even where they depend on its parameters.

<!-- BEGIN GENERATED ITEMS: completion/templates -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent member type**

A variable whose type is a member alias of a dependent specialization completes the members of the class the alias stands for

The alias is resolved with the written template arguments substituted, so `Vec<Vec<T>>::value_type` lists the members of `Vec<T>` rather than nothing at all. An alias naming a reference (`Vec<Vec<T>>::reference`) lists the members of the class referred to, and `->` on a pointer to one lists the pointee's.

```case
tests/completion/templates/01_dependent_member_type
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#443 -->

**Dependent expression results**

A member access on what a subscript or a member call returns inside a template completes the members of the class it evaluates to

`rows[0].` on a `Vec<Vec<T>>` lists the members of `Vec<T>`, followed through the container's `reference` alias the way the standard containers declare it. Where a member has a `const` overload, the constness of the object picks the one called, a data member reached through a const object counting as const, and `->` on a returned iterator reaches the element.

```case
tests/completion/templates/05_dependent_expression_results
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced variable members**

A variable declared `auto` from a dependent initializer completes the members of the class it deduces to

`auto& row = rows[0]; row.` lists the members of `Vec<T>`. The declarator applies as in a real deduction: `const auto&` makes the object const, a by-value `auto` drops the initializer's const, `auto&&` and `decltype(auto)` keep it, and `auto*` takes the pointee. From a data member, `decltype(auto)` takes the type the member is declared with, or with parentheses the const reference the expression is.

```case
tests/completion/templates/07_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#493 clangd#1768 -->

**Generic lambda parameters**

An `auto` parameter of a lambda completes the members of the type the lambda is called with

The call can come after the lambda, or be the algorithm the lambda is
passed to. A lambda called with several types completes nothing.

```case
tests/completion/templates/14_generic_lambda_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1103 -->

**Concept requirements**

Inside a constrained template, the members the concept requires complete after a dot

A member the concept calls completes as a method with the types of the arguments it is called with; after `T::` the nested types it names complete too.

```case
tests/completion/templates/15_concept_requirements
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Members through a dependent specialization**

The members of a dependent specialization of a class template show the arguments the code names it with

After `m.find(1)->` over a `lib::map<int, T>`, `first` is a `const int` and `second` a `T`, not the pair's own `_T1` and `_T2`.

```case
tests/completion/templates/29_library_specialization_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Override and Out-of-line Definitions

Inside a derived class, the virtual functions of its bases complete as whole declarations.

<!-- BEGIN GENERATED ITEMS: completion/overrides -->

<!-- BEGIN CAPABILITY: supported -->

**Override declarations**

Inside a derived class, a base class's virtual function completes as a whole override declaration, return type and `override` included

Inside the override, the name completes as a call, not as another declaration.

```case
tests/completion/overrides/01_override_declaration
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Preprocessor Directives

After the `#` that opens a line, the preprocessor directives complete, and after `#ifdef` the macros defined above it.

<!-- BEGIN GENERATED ITEMS: completion/directives -->

<!-- BEGIN CAPABILITY: supported -->

**Directive names**

After a `#`, the directives complete, and `#elif`, `#else` and `#endif` only inside a conditional block; with `statement_snippets`, each form with placeholders for its arguments

```case
tests/completion/directives/01_directive_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Include Paths

After `#include`, header names complete: a quoted one from the includer's own directory first, then from the include directories, the system's last. Only files that look like headers are candidates: header extensions everywhere, extensionless files in system directories and other places such headers live.

<!-- BEGIN GENERATED ITEMS: completion/include_path_completion -->

<!-- BEGIN CAPABILITY: supported -->

**Quoted include paths**

Completion lists headers and directories from the configured search path and marks directories with a trailing slash

```case
tests/completion/include_path_completion/01_include_quoted
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Angled include paths**

Angled includes offer the same search-path candidates, closed with `>`

```case
tests/completion/include_path_completion/02_include_angled
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Closing delimiter**

Accepting a header closes the directive, replacing what was already typed after the cursor instead of doubling it

A delimiter already there is replaced, and so is the rest of a path when the header is picked in an earlier path component.

```case
tests/completion/include_path_completion/03_closing_delimiter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Headers beside the file**

A quoted include finds the headers next to the including file without any search path

```case
tests/completion/include_path_completion/04_sibling_headers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nearest headers first**

Header names list the includer's directory first, then the quoted include directories, the others in order, the system's last; a name two directories hold is listed once, from the first

```case
tests/completion/include_path_completion/09_header_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Library internals last**

A library's reserved names (`__algorithm/`, `_mingw.h`) come after the other names of their search group, hidden ones (`.git/`) after all the others

```case
tests/completion/include_path_completion/11_reserved_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Import

After `import` or `export import`, module names complete: the unit's own partitions first, then the modules whose interface lies nearest the file.

<!-- BEGIN GENERATED ITEMS: completion/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Import statements**

Known module names complete after `import`, with the closing semicolon inserted

```case
tests/completion/modules/01_import_modules
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Partitions of the current module**

In an implementation unit, the module's own partitions complete as `:partition`, while neither the module itself nor the partitions of other modules are offered

A statement that already ends in a semicolon keeps it.

```case
tests/completion/modules/02_partition_imports
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Partitions in interface units**

In an interface unit, only the module's interface partitions complete

An interface cannot export an internal partition, and names it imports from one may not reach the interface's importers, so internal partitions are offered only to the module's other units.

```case
tests/completion/modules/04_interface_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nearest modules first**

Module names list the modules whose interface lies nearest the file first

```case
tests/completion/modules/09_nearest_modules
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Filtering

What you typed filters the candidates by the starts of their words.

<!-- BEGIN GENERATED ITEMS: completion/filtering -->

<!-- BEGIN CAPABILITY: supported -->

**Underscore filtering**

Underscore-prefixed internal symbols hide unless the typed prefix itself starts with one

```case
tests/completion/filtering/01_underscore_filter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated tagging**

A `[[deprecated]]` candidate is marked deprecated and ranks after its plain sibling

```case
tests/completion/filtering/02_deprecated_tag
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Word-boundary fuzzy match**

Prefix `fb` matches the word starts of `foo_bar_baz`

`frobnicate` is only a weak scattered subsequence of `fb` and is dropped; `foo_bar_baz` matches on the `foo` and `bar` word boundaries and survives.

```case
tests/completion/filtering/03_fuzzy_word_boundary
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Prefix outranks subsequence**

An exact-prefix candidate sorts above a scattered subsequence match

For prefix `fo`, `format_output` is a true prefix and outscores `fast_math_operation`, which only matches as a subsequence.

```case
tests/completion/filtering/05_prefix_beats_subsequence
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Completion inside a word**

Completing in the middle of a word offers both ranges: the editor either inserts before the rest of the word or replaces the whole word

```case
tests/completion/filtering/06_inside_a_word
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Ranking

Among the candidates left, the names closest to the code come first, and those of the type the code expects.

<!-- BEGIN GENERATED ITEMS: completion/ranking -->

<!-- BEGIN CAPABILITY: supported -->

**Closest names first**

Among the names a prefix matches, locals come first, then the class's members, then the file's names, macros last

```case
tests/completion/ranking/01_closest_first
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expected type first**

A name of the type the code expects ranks above the others its prefix matches

```case
tests/completion/ranking/02_expected_type
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Trigger Characters

Registered: `. < > : " /` and space.

| Character | Context        | Behavior                                            |
| --------- | -------------- | --------------------------------------------------- |
| `.`       | Member access  | Semantic completion                                 |
| `>`       | Via `->`       | Pointer member completion; any other `>` is ignored |
| `:`       | Via `::`       | Scope completion                                    |
| `<`       | `#include <`   | Include path completion                             |
| `"`       | `#include "`   | Include path completion                             |
| `/`       | Path separator | Include path continuation                           |
| ` `       | After `import` | Module name completion (extension-gated)            |
