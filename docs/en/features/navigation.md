# Code Navigation

## Go to Definition

<!-- BEGIN GENERATED ITEMS: go_to_definition -->

<!-- BEGIN CAPABILITY: supported -->

**Cross-TU go-to-definition**

A use in one translation unit resolves to the definition supplied by
a sibling source — the answer spans the project, not the current
file alone.

```snap
tests/snap/navigation/go_to_definition/01_def_cross_tu/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definition and declaration alternate at the cursor site**

On a use, go-to-definition reaches the definition. Invoked on the
definition it steps to the declaration, and on the declaration it
steps to the definition — the two sites alternate. A symbol defined
inline, with no separate declaration, keeps its definition as the
answer.

```snap
tests/snap/navigation/go_to_definition/02_def_decl_alternate.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration-only symbols navigate to their declaration**

Symbols that carry only a declaration — pure virtuals, `extern`
variables, in-class static constants — resolve to that declaration
instead of returning nothing.

```snap
tests/snap/navigation/go_to_definition/03_def_declaration_only.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Go-to-definition on `#include` directives**

Invoked on an `#include` line, go-to-definition opens the included
file. This works for the leading includes compiled into the preamble
(the PCH) as well as ordinary ones later in the file.

```snap
tests/snap/navigation/go_to_definition/04_def_include/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Local variables and parameters navigate to their declaration**

Go-to-definition on a local variable or parameter jumps to its
declaration inside the function body.

```snap
tests/snap/navigation/go_to_definition/05_def_local_symbol.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Navigate through macro wrappers to the underlying declaration**

A name spelled in a macro argument anchors at its spelling, so
definition and declaration alternate there exactly as at a plain
site, and a later use resolves through the wrapper to the function it
declares.

```snap
tests/snap/navigation/go_to_definition/06_def_macro_wrapper.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Names conjured by a macro body or token paste anchor at the invocation**

A name assembled by token paste has no spelling of its own in the
source, so it anchors at the macro invocation that creates it: the
invocation is its definition site, and a plain use of the name jumps
back to that invocation.

```snap
tests/snap/navigation/go_to_definition/07_def_macro_generated.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Tokens inside a `#define` body carry no navigation of their own**

A token written inside a macro body has no meaning until an expansion
assigns one, so navigation on it yields nothing, while the invocation
token always resolves to the macro being expanded.

```snap
tests/snap/navigation/go_to_definition/08_def_macro_body.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Error recovery**

Navigate to a variable whose type is unresolved

When a variable's type name fails to resolve, go-to-definition on a
later use of the variable currently returns nothing, even though the
variable's own declaration is still recorded.

```snap
tests/snap/navigation/go_to_definition/09_def_error_recovery.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent member navigation in uninstantiated templates**

Inside a template that is never instantiated, a member accessed on an
object of a dependent type resolves to the member declared on the
corresponding class template.

```snap
tests/snap/navigation/go_to_definition/10_def_dependent_type.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#212 -->

**Template specialization navigates to the primary template**

Go-to-definition on the name of an explicit specialization resolves to
the specialization itself; stepping from it to the primary template it
specializes is not offered.

```snap
tests/snap/navigation/go_to_definition/11_def_template_spec.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2055 -->

**`auto` keyword navigates to the deduced type**

Go-to-definition on the `auto` keyword should reach the type it was
deduced to; today it returns nothing.

```snap
tests/snap/navigation/go_to_definition/12_def_auto_keyword.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Implicit targets

Navigate to definitions of implicitly invoked code. In C++ many constructs generate hidden calls to constructors, operators, conversions, etc. Navigating from the syntactic construct (a brace, a keyword, an operator token) to the actual function being called is essential for understanding what code is really executing.

Implicit navigation requires an unambiguous source token — patterns where the token already has a well-defined go-to-def target (e.g., a variable name always goes to its declaration) cannot be repurposed for implicit call navigation.

<!-- BEGIN GENERATED ITEMS: implicit_targets -->

<!-- BEGIN CAPABILITY: unsupported -->

**`override` / `final`**

Navigate to the overridden base method

Go-to-definition on the `override` or `final` specifier should reach the
base class virtual method it overrides; today it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/01_implicit_override_final.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**`break` / `continue`**

Navigate to the enclosing loop or switch head

Go-to-definition on `break` or `continue` should reach the head of the
loop or switch it controls; today it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/02_implicit_break_continue.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructor calls**

From parentheses or braces to the selected constructor

Go-to-definition on the opening parenthesis or brace of a constructor
call reaches the constructor overload resolution selected, for both the
`T(args)` and `T{args}` forms.

```snap
tests/snap/navigation/implicit_code_navigation/03_implicit_constructor_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Copy/move construction and assignment**

To the constructor or assignment operator

Go-to-definition on the `=` of an assignment reaches the assignment
operator. The `=` that introduces a copy- or move-initialization
(`T b = a;`) is initialization syntax rather than an operator call and is
not yet resolved.

```snap
tests/snap/navigation/implicit_code_navigation/04_implicit_copy_move.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**CTAD**

Navigate to the selected constructor

When class template argument deduction picks a specialization, go-to-
definition on the constructor call reaches the constructor that was
selected, not merely the class template.

```snap
tests/snap/navigation/implicit_code_navigation/05_implicit_ctad.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Aggregate initialization**

Navigate to the struct definition

An aggregate has no constructor, so go-to-definition on its initializer
brace reaches the aggregate's definition.

```snap
tests/snap/navigation/implicit_code_navigation/06_implicit_aggregate_init.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`delete` expression**

Navigate to the destructor

Go-to-definition on `delete` should reach the destructor it runs; today
it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/07_implicit_delete_dtor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`new` expression**

Navigate to the constructor and overloaded `operator new`

Go-to-definition on `new` reaches the class's overloaded `operator new`.
The constructor invoked by the same expression is not part of the reply.

```snap
tests/snap/navigation/implicit_code_navigation/08_implicit_new_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Member initializer list**

Navigate to base and member constructors

The base and member constructors run by an initializer list are reached
from the opening parenthesis of each initializer. The initializer name
itself resolves to the base type or the member, so navigation to the
constructor goes through the parenthesis.

```snap
tests/snap/navigation/implicit_code_navigation/09_implicit_member_init.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Delegating constructors**

Navigate to the target constructor

A delegating constructor's target is reached from the opening parenthesis
of the delegated call. The constructor name itself resolves to the class
type, so navigation to the target constructor goes through the
parenthesis.

```snap
tests/snap/navigation/implicit_code_navigation/10_implicit_delegating_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Inherited constructors**

Navigate to the base constructors brought in by `using`

Go-to-definition on an inherited-constructor declaration
(`using Base::Base;`) reaches a base constructor. When the base declares
several constructors the reply resolves to one of them rather than
listing the whole set.

```snap
tests/snap/navigation/implicit_code_navigation/11_implicit_inherited_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Return value implicit construction**

Navigate to the constructor

A braced `return {args}` implicitly constructs the function's return
type; go-to-definition on the brace reaches the selected constructor.

```snap
tests/snap/navigation/implicit_code_navigation/12_implicit_return_construction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Lambda init-capture**

Navigate to the constructor

Go-to-definition on the `=` of a lambda init-capture should reach the
constructor that builds the captured value; today it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/13_implicit_lambda_capture.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overloaded operators**

From the operator token to its definition

Go-to-definition on an overloaded operator token reaches the operator's
definition. The binary, subscript, call and arrow operators (`+`, `[]`,
`()`, `->`) are all resolved.

```snap
tests/snap/navigation/implicit_code_navigation/14_implicit_operator_call.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C++20 rewritten operators**

Navigate to the operator the rewrite uses

For a comparison synthesized by the C++20 rewrite rules, go-to-definition
on the written operator reaches the operator that actually implements it:
`!=` reaches `operator==`, and `>` reaches `operator<=>`.

```snap
tests/snap/navigation/implicit_code_navigation/15_implicit_rewritten_operator.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**User-defined literals**

Navigate to the literal operator

Go-to-definition on a user-defined-literal suffix should reach its
`operator""`; today it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/16_implicit_udl.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1931 -->

**Implicit conversion operators**

From a conversion context to the operator

Go-to-definition from a context that runs a user-defined conversion (a
condition, `!`, an explicit `bool(...)`) should reach the conversion
operator; today it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/17_implicit_conversion_context.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Casts invoking a constructor or conversion operator**

A `static_cast` that constructs its target reaches the selected
constructor. A `static_cast` that runs a user-defined conversion operator
does not yet reach the operator.

```snap
tests/snap/navigation/implicit_code_navigation/18_implicit_cast_conversion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Range-based for**

Navigate to `begin()` / `end()`

Go-to-definition on the `:` of a range-based for should reach the
`begin()` / `end()` chosen for the range; today it returns nothing.

```snap
tests/snap/navigation/implicit_code_navigation/19_implicit_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Structured bindings**

Navigate to the underlying accessors or fields

Go-to-definition on a structured binding name resolves to the binding
itself rather than the underlying field or accessor it names.

```snap
tests/snap/navigation/implicit_code_navigation/20_implicit_structured_binding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`co_await` / `co_yield` / `co_return`**

Navigate to the awaiter or promise method

Go-to-definition on `co_yield` reaches the promise's `yield_value`. The
`co_await` and `co_return` keywords do not yet reach the awaiter's or
promise's methods.

```snap
tests/snap/navigation/implicit_code_navigation/21_implicit_coroutine.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Template navigation

Navigation inferred from initialization and decomposition patterns, including
class template argument deduction.

<!-- BEGIN GENERATED ITEMS: template_navigation -->

<!-- END GENERATED ITEMS -->

## Go to Declaration

Navigate from a symbol usage or definition to its declaration. In C++, many entities have separate declarations and definitions.

clice returns the declaration locations plus the definition — symbols defined inline have no separate declaration — minus the site the cursor already stands on, so declaration and definition sites alternate just like go-to-definition.

<!-- BEGIN GENERATED ITEMS: go_to_declaration -->

<!-- BEGIN CAPABILITY: supported -->

**Cross-TU go-to-declaration**

Go-to-declaration on a use resolves sites in other files: the
prototype lives in a shared header and the out-of-line definition in a
sibling source, and both are offered from a use in another file.

```snap
tests/snap/navigation/go_to_declaration/01_decl_cross_tu/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functions**

From a use or out-of-line definition to the prototype

Go-to-declaration reaches a function's prototype both from a call site
and from the out-of-line definition — the two non-cursor sites the
prototype alternates with.

```snap
tests/snap/navigation/go_to_declaration/02_decl_function_prototype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Forward declarations of classes and structs**

A class with a forward declaration and a later definition offers both
from a use — the forward declaration stays part of the declaration set
rather than being dropped in favour of the definition.

```snap
tests/snap/navigation/go_to_declaration/03_decl_forward_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static data member**

To the in-class declaration

A static data member is declared inside the class and defined out of
line; go-to-declaration on a use offers the in-class declaration
alongside the definition.

```snap
tests/snap/navigation/go_to_declaration/04_decl_static_member.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`extern` variable**

To the declaration

A use of an `extern` variable offers the `extern` declaration and
the defining declaration together, so the header-side declaration is
always reachable from a use.

```snap
tests/snap/navigation/go_to_declaration/05_decl_extern_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Multiple declarations**

Every declaration site

When an entity is declared in several places, go-to-declaration on a
use lists every declaration site, not only the nearest one.

```snap
tests/snap/navigation/go_to_declaration/06_decl_multiple.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration and definition with cosmetically different signatures**

Parameter names, and a top-level `const` on a parameter, are not part
of a function's type: the declaration and the definition below spell the
same function differently, yet go-to-declaration still connects a use to
the prototype.

```snap
tests/snap/navigation/go_to_declaration/07_decl_signature_mismatch.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Implementation

<!-- BEGIN GENERATED ITEMS: go_to_implementation -->

<!-- BEGIN CAPABILITY: supported -->

**Override chain**

Each level of a chain to its own overriders

Along a three-level override chain, go-to-implementation from each method
reaches the override one level down — base to middle, middle to leaf.

```snap
tests/snap/navigation/go_to_implementation/01_impl_virtual_chain.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Sibling overrides**

Every sibling override

Go-to-implementation on a virtual method lists every override across
the sibling derived classes.

```snap
tests/snap/navigation/go_to_implementation/02_impl_virtual_siblings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#854 -->

**Non-virtual function**

Declaration to out-of-line definition

Go-to-implementation on a non-virtual function declaration should reach
its out-of-line definition, behaving as a superset of go-to-definition;
today it returns nothing.

```snap
tests/snap/navigation/go_to_implementation/03_impl_nonvirtual_def.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Base class**

Every derived class

Go-to-implementation on a base class name lists the classes that derive
from it.

```snap
tests/snap/navigation/go_to_implementation/04_impl_base_derived.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Template duck-type navigation**

From a dependent member call, go-to-implementation should list the
concrete methods of every known instantiation; the same applies to a
generic lambda's dependent calls. Today it returns nothing.

```snap
tests/snap/navigation/go_to_implementation/05_impl_template_duck_type.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Type Definition

Navigate to the type definition of a symbol. Applicable to variables, parameters, fields, and any other named entity that has a type. When the type is a type alias or a pointer-like wrapper, navigation should unwrap to the underlying/pointee type.

<!-- BEGIN GENERATED ITEMS: go_to_type_definition -->

<!-- BEGIN CAPABILITY: supported -->

**Variables and parameters**

Go-to-type-definition on a local variable or a parameter reaches the
definition of its type.

```snap
tests/snap/navigation/go_to_type_definition/01_typedef_variables.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class and struct fields**

Go-to-type-definition on a field access reaches the definition of the
field's type.

```snap
tests/snap/navigation/go_to_type_definition/02_typedef_field.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`auto`-deduced variables**

Go-to-type-definition on an `auto`-deduced variable should reach the
deduced type's definition; today the variable carries no type relation,
so it returns nothing.

```snap
tests/snap/navigation/go_to_type_definition/03_typedef_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1026 -->

**Smart pointer to the pointee type**

Go-to-type-definition on a smart-pointer variable reaches the wrapper
type itself; unwrapping to the pointee type is not offered.

```snap
tests/snap/navigation/go_to_type_definition/04_typedef_smart_pointer.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Type aliases**

Go-to-type-definition on a variable of an aliased type reaches the
`using` or `typedef` declaration; it does not yet unwrap the alias to
the underlying type's definition.

```snap
tests/snap/navigation/go_to_type_definition/05_typedef_alias.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured binding variables**

Go-to-type-definition on a structured binding reaches the definition of
the bound member's type.

```snap
tests/snap/navigation/go_to_type_definition/06_typedef_structured_binding.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Find References

<!-- BEGIN GENERATED ITEMS: find_references -->

<!-- BEGIN CAPABILITY: supported -->

**Cross-TU find references**

Find references gathers uses from other files too: a function
defined in one source and called from a sibling reports both call
sites together with the declaration in the shared header, not only the
uses in the current file.

```snap
tests/snap/navigation/find_references/01_refs_cross_tu/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration and definition sites appear among references**

A reference query returns the declaration and the out-of-line
definition together with every use, so the whole surface of a symbol
is reachable from any one of its sites.

```snap
tests/snap/navigation/find_references/02_refs_include_declaration.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1081 -->

**Implicit references from range-based for loops**

Find references on `begin` reports only its own declaration; the
range-based for loop that implicitly calls it is not included among the
references.

```snap
tests/snap/navigation/find_references/03_refs_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Implicit constructor and destructor calls**

Find references on a constructor reports only its explicit sites; an
object definition that implicitly invokes the constructor or its
destructor is not included.

```snap
tests/snap/navigation/find_references/04_refs_implicit_construction.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 clangd#1872 -->

**References through forwarding functions**

Find references on a constructor does not include call sites that reach
it indirectly through a perfect-forwarding factory.

```snap
tests/snap/navigation/find_references/05_refs_forwarding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#258 clangd#675 -->

**References in dependent and template contexts**

Find references on a member does not include dependent call sites in a
template, even when the template is instantiated with the member's
class.

```snap
tests/snap/navigation/find_references/06_refs_dependent_context.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2139 -->

**Read/write classification of references**

The reference reply carries only locations, so a reader cannot tell a
write from a read; annotating each result with its access kind is not
offered.

```snap
tests/snap/navigation/find_references/07_refs_read_write.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#177 -->

**Enclosing function shown with each reference**

Each reference is reported as a bare location; the name of the function
that encloses it is not attached, so results carry no context beyond
the file and line.

```snap
tests/snap/navigation/find_references/08_refs_enclosing_context.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro references across expansions, `#ifdef`/`#ifndef` and `#undef`**

A macro's references span its expansions, the `#ifdef` / `#ifndef`
conditionals that test it and the `#undef` that cancels it. Each
`#define` of a name is its own symbol, so a redefinition after `#undef`
collects only its own uses.

```snap
tests/snap/navigation/find_references/09_refs_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#346 -->

**Macro references spelled inside other macro definitions**

Find references on a macro does not include the mentions of it written
inside the bodies of other macro definitions.

```snap
tests/snap/navigation/find_references/10_refs_macro_in_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Label and goto references**

Find references on a label lists the label itself together with every
`goto` that jumps to it.

```snap
tests/snap/navigation/find_references/11_refs_label_goto.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Call Hierarchy

<!-- BEGIN GENERATED ITEMS: call_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Prepare call hierarchy on functions and methods**

Preparing a call hierarchy works on a free function and on a member
method alike, anchoring an item at the entity under the cursor.

```snap
tests/snap/navigation/call_hierarchy/01_calls_prepare.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Incoming calls**

Incoming calls list every caller of a function, and a caller that
invokes it more than once contributes each call site.

```snap
tests/snap/navigation/call_hierarchy/02_calls_incoming.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Outgoing calls**

Outgoing calls list every function a body invokes, one entry per
callee.

```snap
tests/snap/navigation/call_hierarchy/03_calls_outgoing.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Function signature in the item detail**

A call hierarchy item carries only its name; the function signature is
not attached in a detail field, so overloads are indistinguishable in
the hierarchy.

```snap
tests/snap/navigation/call_hierarchy/04_calls_detail_signature.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Qualified name for member functions**

A member function's call hierarchy item is produced, but its name field
carries only the bare method name (`draw`), not the qualified
`Circle::draw` that would tell it apart from a free function.

```snap
tests/snap/navigation/call_hierarchy/05_calls_qualified_name.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Follow virtual dispatch**

Incoming calls of a base virtual method do not include calls made
through derived overrides; a call to an override is attributed only to
that override, never to the base it overrides.

```snap
tests/snap/navigation/call_hierarchy/06_calls_virtual_dispatch.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1308 -->

**Non-function targets**

Variables and enum constants

Preparing a call hierarchy on a variable or an enum constant returns
nothing; the request is offered only for functions and methods.

```snap
tests/snap/navigation/call_hierarchy/07_calls_non_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Calls inside lambdas**

A call written in a lambda body appears in the incoming calls of the
function it invokes, attributed to the function that encloses the
lambda.

```snap
tests/snap/navigation/call_hierarchy/08_calls_lambda.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2242 -->

**Constructor calls through forwarding functions**

Incoming calls of a constructor do not include the call sites that
reach it through a perfect-forwarding factory.

```snap
tests/snap/navigation/call_hierarchy/09_calls_forwarding_ctor.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Hierarchy

<!-- BEGIN GENERATED ITEMS: type_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Prepare type hierarchy on class, struct, enum and union**

Preparing a type hierarchy anchors an item on any user-defined type
tag — class, struct, enum and union alike.

```snap
tests/snap/navigation/type_hierarchy/01_types_prepare.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Supertypes**

Supertypes list every direct base of a class, including each base of a
multiple-inheritance derived type.

```snap
tests/snap/navigation/type_hierarchy/02_types_supertypes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Subtypes**

Subtypes list every class that derives from a base, across sibling
derived types.

```snap
tests/snap/navigation/type_hierarchy/03_types_subtypes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template inheritance**

Subtypes of a base include classes that derive from it through a class
template, such as a CRTP wrapper.

```snap
tests/snap/navigation/type_hierarchy/04_types_template_inheritance.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#31 -->

**Template arguments in type hierarchy items**

A subtype produced by a class template specialization is listed, but
its item name carries only the bare template name (`Derived`), without
the template arguments that would distinguish `Derived<Foo>`.

```snap
tests/snap/navigation/type_hierarchy/05_types_template_args.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Workspace Symbol

Search the whole project for a symbol by name (`workspace/symbol`).

<!-- BEGIN GENERATED ITEMS: workspace_symbol -->

<!-- BEGIN CAPABILITY: supported -->

**Basic workspace-wide symbol search**

case-insensitive substring matching

A query matches any symbol whose name contains it, ignoring case:
functions, types, enumerators and macros all participate, and a query
with no match returns an empty list rather than an error.

```snap
tests/snap/workspace_symbol/workspace_symbol/01_basic_search.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Search spans the whole project**

Hits from files other than the queried one

The query returns symbols from project files that are not even open
in the editor: `other.h` stays closed here, so its hit is served by
the background index.

```snap
tests/snap/workspace_symbol/workspace_symbol/02_cross_file_search/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1344 -->

**Overload disambiguation**

Parameter types shown in results

Querying an overloaded name finds every overload, but each entry
carries only the bare name — nothing tells the two `process` results
apart short of opening both locations.

```snap
tests/snap/workspace_symbol/workspace_symbol/03_overload_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#914 -->

**Fuzzy matching**

word-boundary-aware scoring for camelCase and snake_case

Matching is a case-insensitive substring test: `LinLis` does not find
`LinkedList`, and `pcfg` does not find `parse_config`. Word-boundary
initials should match and score for every symbol kind, macros
included.

```snap
tests/snap/workspace_symbol/workspace_symbol/04_fuzzy_matching.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#550 -->

**Partially qualified name search**

Symbols match by bare name only: `net::Socket` finds nothing even
though `deep::net::Socket` exists, and neither does any other
qualifier-prefixed form.

```snap
tests/snap/workspace_symbol/workspace_symbol/05_qualified_search.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#931 -->

**Enumerator lookup under the enum's scope**

`Color::Red` should find the enumerator — for scoped and unscoped
enums alike — but qualified queries match nothing; only the bare
`Red` does.

```snap
tests/snap/workspace_symbol/workspace_symbol/06_enum_scope.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2253 -->

**Underlying declarations ranked above type aliases**

When both `ConnectionImpl` and its alias `Connection` match a query,
the underlying declaration should rank first. Results carry no
ranking today.

```snap
tests/snap/workspace_symbol/workspace_symbol/07_alias_priority.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Search by mangled (linker) name**

Pasting a linker symbol such as `_Z7processi` should resolve to the
function it mangles — useful when chasing linker errors and stack
traces.

```snap
tests/snap/workspace_symbol/workspace_symbol/08_mangled_name.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Navigation

<!-- BEGIN GENERATED ITEMS: module_navigation -->

<!-- BEGIN CAPABILITY: supported clangd#2310 -->

**`import module_name` navigates to the module interface unit**

Go-to-definition on the name in an `import` declaration opens the
module interface unit that exports it, and uses of an imported symbol
reach its definition in that unit.

```snap
tests/snap/navigation/module_navigation/01_module_import_name/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`import :partition` navigates to the partition unit**

Go-to-definition on the partition name after the colon in a partition
import opens the partition unit that declares it.

```snap
tests/snap/navigation/module_navigation/02_module_partition_import/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Navigate between interface and implementation units of one module**

Go-to-definition on the module name in an implementation unit
(`module m;`) jumps to the interface unit that declares the module;
the reverse direction, from the interface name to the implementation,
is not offered.

```snap
tests/snap/navigation/module_navigation/03_module_iface_impl/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Dot-separated module name**

Navigate each segment

Go-to-definition on the leading segment of a dot-separated module name
reaches the module's interface unit; the segments after a dot do not
resolve on their own yet.

```snap
tests/snap/navigation/module_navigation/04_module_dotted/main.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Document Highlight

Highlight all references to the symbol under cursor within the current file (`textDocument/documentHighlight`).

<!-- BEGIN GENERATED ITEMS: document_highlight -->

<!-- BEGIN CAPABILITY: unsupported -->

**Highlight every reference to the symbol under the cursor in the current file**

Placing the cursor on `total` should light up its declaration and
every use in the file; the request is not implemented.

```snap
tests/snap/navigation/document_highlight/01_highlight_references.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Read/write classification for symbol highlights**

Each highlight should carry its access kind, so editors can tint
writes differently from reads.

```snap
tests/snap/navigation/document_highlight/02_highlight_read_write.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**Control flow token highlighting**

Highlighting `break` or `continue` should also light up the loop or
`switch` it belongs to — and `return` / `throw` the function exits
they mark.

```snap
tests/snap/navigation/document_highlight/03_highlight_control_flow.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Switch Source/Header

<!-- BEGIN GENERATED ITEMS: switch_source_header -->

<!-- BEGIN CAPABILITY: unsupported -->

**Switch between a source file and its header**

From `widget.cpp` a single command should jump to `widget.h` and
back — the `textDocument/switchSourceHeader` request clangd clients
rely on is not implemented.

```snap
tests/snap/navigation/switch_source_header/01_switch_source_header.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
