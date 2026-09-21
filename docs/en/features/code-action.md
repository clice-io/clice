# Code Action

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/code_action/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice offers code actions on a selection: refactorings that generate or reshape code from what the compiler knows about it, and quick fixes for names no header declares. Every action is computed to completion when it is offered, so applying one never waits on a second request; edits carry the document version they were computed for, and an editor refuses them once the buffer moved on. The same actions run headless through `clice inspect code_action`.

An action anchors on the innermost construct the selection covers — a method declaration, a class name, a `switch`, an `auto` — so the list stays short: a click on a method name offers what applies to that method, a click on the class name what applies to the class.

## Defining Functions

<!-- BEGIN GENERATED ITEMS: define -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**Define a declared method**

A method declaration offers an inline body and an out-of-line definition after its class

The out-of-line definition repeats the declaration with `S::` before the
name and drops what belongs to the declaration alone, such as default
arguments.

```snap
tests/snap/code_action/define/01_method_out_of_line.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration-only specifiers dropped**

`virtual`, `static`, `explicit`, `override` and `final` do not appear on the definition

Specifiers that must stay, such as `constexpr` and `noexcept`, are kept.

```snap
tests/snap/code_action/define/02_declaration_specifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definition-scope return type**

A return type naming a nested type or another namespace is spelled so it resolves at the definition

Parameter types are looked up in the class scope like the declaration's
and stay as written.

```snap
tests/snap/code_action/define/03_qualified_return_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class template member**

The definition of a class template's member carries the template head and the template arguments in its qualifier

Default template arguments are not repeated on the head.

```snap
tests/snap/code_action/define/04_class_template_member.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Member function template**

A member template keeps its own template head, minus its default arguments, after the class's

Constrained templates keep their requires-clauses, which a definition must
repeat.

```snap
tests/snap/code_action/define/05_member_template.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructors, destructors and operators**

Special member functions are defined under the class name they are spelled with

Conversion functions and operators keep their full spelling.

```snap
tests/snap/code_action/define/06_special_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Free function declaration**

A declared free function is defined right after its declaration, in the same namespace

The name needs no qualifier inside the namespace; the return type is
spelled for that scope.

```snap
tests/snap/code_action/define/07_free_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Placement after existing definitions**

When the class already has out-of-line definitions in the file, the new one goes after the last of them

The qualifier follows the scope of that definition, not the class's.

```snap
tests/snap/code_action/define/08_placement_after_definitions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**Define all missing members**

On the class name, every member function without a definition is defined at once, in declaration order

Members that already have a definition, pure virtuals and defaulted
members are left alone.

```snap
tests/snap/code_action/define/09_missing_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Missing members from a definition**

Inside an out-of-line definition, the class's remaining undefined members are offered too

This is how a source file completes a class declared elsewhere: the
definitions join the ones already there.

```snap
tests/snap/code_action/define/10_missing_from_definition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**Define in the host source**

In a header, a member can also be defined in the source file the header is compiled with

The definition is fully qualified and joins the class's other
definitions in that file; members already defined in some source file
are not offered again. Templates and inline functions stay in the header.

```snap
tests/snap/code_action/define/11_header_host/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested class member**

A nested class's member is defined after the outermost enclosing class, qualified through every level

```snap
tests/snap/code_action/define/12_nested_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Dependent return type**

A dependent return type stays as written, which may need `typename` and qualification outside the class

```snap
tests/snap/code_action/define/13_dependent_return_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declarations under a linkage specification**

Functions declared with C linkage are defined like any other, inside the linkage block or after a single-declaration form

```snap
tests/snap/code_action/define/14_linkage_specification.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Implementing Interfaces

<!-- BEGIN GENERATED ITEMS: implement -->

<!-- BEGIN CAPABILITY: supported clangd#1037 -->

**Implement pure virtual methods**

A class deriving from an abstract base receives an `override` declaration for each unimplemented pure virtual method

The declarations go at the end of the class body.

```snap
tests/snap/code_action/implement/01_pure_virtuals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pure virtuals through a chain**

Only the methods no class in the chain implemented are declared, and a `class` gets a `public:` label for them

```snap
tests/snap/code_action/implement/02_inheritance_chain.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Types for the derived class**

Parameter and return types written in the base's scope are qualified so they resolve in the derived class

Reference qualifiers and constness are carried over.

```snap
tests/snap/code_action/implement/03_qualified_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Conversion functions and pointer parameters**

A conversion function has no return type to print, and a parameter whose type wraps its name keeps that shape

```snap
tests/snap/code_action/implement/04_conversion_and_pointers.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Switch Cases

<!-- BEGIN GENERATED ITEMS: switch_cases -->

<!-- BEGIN CAPABILITY: supported clangd#807 -->

**Missing enum cases**

A switch over an enum receives the enumerators it does not handle, followed by a `break`

Enumerators sharing a handled value are considered covered.

```snap
tests/snap/code_action/switch_cases/01_missing_cases.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Cases before the default**

With a `default` present, the missing cases go right before it and fall through into it, keeping the behavior

The action is offered from anywhere inside the switch.

```snap
tests/snap/code_action/switch_cases/02_before_default.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unscoped enum in a namespace**

Enumerators of an unscoped enum are qualified with the enum's namespace when the switch lies outside it

```snap
tests/snap/code_action/switch_cases/03_unscoped_enum.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Complete switch offers nothing**

A switch handling every enumerator, or one over a non-enum value, offers no action

```snap
tests/snap/code_action/switch_cases/04_complete_switch.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Selection covering the switch**

A selection spanning the whole statement offers the same action as a cursor inside it

```snap
tests/snap/code_action/switch_cases/05_selection_range.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Deduced Types

<!-- BEGIN GENERATED ITEMS: deduced_type -->

<!-- BEGIN CAPABILITY: supported -->

**Expand auto in declarations**

`auto` in a variable declaration is replaced by the type it deduced, leaving qualifiers and declarators in place

```snap
tests/snap/code_action/deduced_type/01_auto_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced return type**

A function's deduced `auto` return type expands to the deduced type, spelled for the function's scope

```snap
tests/snap/code_action/deduced_type/02_auto_return.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expand decltype**

A `decltype` specifier expands to the type it denotes

```snap
tests/snap/code_action/deduced_type/03_decltype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unnameable types stay auto**

Lambdas, dependent types and other types without a spelling are not expanded

```snap
tests/snap/code_action/deduced_type/04_unnameable_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Forwarding references and declarator types**

`auto&&` bound to an lvalue takes the deduced reference in place of both tokens, and a type that wraps the name is left alone

```snap
tests/snap/code_action/deduced_type/05_forwarding_reference.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

<!-- BEGIN GENERATED ITEMS: macro -->

<!-- BEGIN CAPABILITY: supported clangd#820 -->

**Expand a macro invocation**

A macro invocation is replaced by the tokens it expands to

Arguments are substituted; the action is offered from the macro name
or anywhere inside its arguments.

```snap
tests/snap/code_action/macro/01_expand_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested macros expand fully**

A macro whose body invokes other macros expands to the final tokens

```snap
tests/snap/code_action/macro/02_nested_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Directive references and empty macros**

A macro named in a preprocessor condition is not an expansion to replace, while a macro expanding to nothing is deleted

```snap
tests/snap/code_action/macro/03_directives_and_empty.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Missing Includes

<!-- BEGIN GENERATED ITEMS: include -->

<!-- BEGIN CAPABILITY: supported clangd#1017 -->

**Standard library include**

An unresolved standard library name offers the header declaring it, from the standard library mapping

The directive goes after the file's last include. An unqualified name
also tries the `std` namespace.

```snap
tests/snap/code_action/include/01_standard_library.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include for a project symbol**

A name declared in a project header the file does not include offers that header, spelled relative to the file

The candidates come from the project index: `lib.h` is known because
another source file includes it.

```snap
tests/snap/code_action/include/02_project_header/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**First include of a file**

A file without includes receives the directive at its start, after a `#pragma once` when there is one

```snap
tests/snap/code_action/include/03_no_include_yet.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Reordering Definitions

<!-- BEGIN GENERATED ITEMS: reorder -->

<!-- BEGIN CAPABILITY: supported -->

**Reorder definitions by declaration**

The out-of-line definitions of a class's members are reordered to follow the declaration order in the class

Each definition moves with the comment block directly above it.

```snap
tests/snap/code_action/reorder/01_class_members.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespace blocks reorder separately**

Definitions written in different namespace blocks are reordered within each block, never across

```snap
tests/snap/code_action/reorder/02_namespace_blocks.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Free functions by declaration order**

From a free function's definition, the definitions of the functions declared alongside it are reordered as declared

```snap
tests/snap/code_action/reorder/03_free_functions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Trailing comments stay in place**

A comment ending a definition's line moves with that definition, never with the one below it

```snap
tests/snap/code_action/reorder/04_trailing_comments.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Constructors

<!-- BEGIN GENERATED ITEMS: constructor -->

<!-- BEGIN CAPABILITY: supported -->

**Memberwise constructor**

A class receives a constructor taking every field in order, scalars by value and other types by const reference

```snap
tests/snap/code_action/constructor/01_memberwise.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Existing constructor not duplicated**

No constructor is generated when the class already declares one taking as many arguments as it has fields

```snap
tests/snap/code_action/constructor/02_existing_constructor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Single field is explicit**

A one-field class gets an `explicit` constructor, placed under a `public:` label when the class ends in another section

```snap
tests/snap/code_action/constructor/03_single_field.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declarator-shaped field types**

A field whose type wraps the name, such as a function pointer, keeps that shape in the parameter

```snap
tests/snap/code_action/constructor/04_declarator_fields.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Base without a default constructor**

No constructor is generated when a base class needs its own initializer, since the memberwise one initializes fields alone

```snap
tests/snap/code_action/constructor/05_base_without_default.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Formatting

Generated text is formatted with the project's clang-format style when one applies to the file, so a definition or a block of `case` labels lands in the surrounding code's layout. Without a style, or with `DisableFormat`, the text keeps the layout shown in the examples above.

## Known Limitations

- A definition placed in the host source goes after the last definition of the class's members the index knows in that file, or at the end of the file when it holds none. Which source file hosts a header follows the header's compilation context.
- Members already defined in another source file are left out of a "define missing members" action only when the project index knows that definition; with indexing disabled every undefined member is offered.
- A dependent return type is copied as written into an out-of-line definition, where it may need `typename` and the class qualifier.
- Missing-include candidates come from the standard library mapping and from headers the project index has seen; a header no indexed source file includes is not suggested.

## Not Implemented

Quick fixes from compiler and clang-tidy fix-it hints, extract function and variable, inline function and variable, moving a definition between header and source, converting an unscoped enum to a scoped one, and changing a function's signature across its callers.
