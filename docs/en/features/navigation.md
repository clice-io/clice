# Code Navigation

## Go to Definition

Go to definition jumps from a name to the definition of what it names, and from a definition back to its declaration.

<!-- BEGIN GENERATED ITEMS: navigation/definition -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration-only symbols**

A symbol with no definition in sight resolves to its declaration

```case
tests/navigation/definition/01_declaration_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Partition imports**

A name a partition exports resolves into that partition, and so does the partition's name in its import

```case
tests/navigation/definition/02_partition_import
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration and definition alternate**

Navigation from a use reaches the definition, and from the declaration or the definition the other one

A function declared and defined apart answers each request at its
declaration or definition with the other site. A function with no separate
declaration answers with its definition everywhere.

```case
tests/navigation/definition/03_declaration_and_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Go-to-definition on `#include` directives**

Go-to-definition on an include opens the referenced file

Leading includes and ordinary includes later in the file behave alike.

```case
tests/navigation/definition/05_include_directives
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Local symbol navigation**

Go-to-definition on a local variable or parameter jumps to its declaration inside the function body

```case
tests/navigation/definition/06_local_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Unresolved variable types**

A variable whose type does not resolve can not be navigated to from its uses yet

Go-to-definition on a later use of the variable returns nothing, although
its declaration is right there.

```case
tests/navigation/definition/07_error_recovery
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pure virtual functions**

Go-to-definition on the declaration of a pure virtual function lists its overrides

Nothing defines a pure virtual function: standing on its declaration,
go-to-definition reaches the overrides that do. A call through it reaches
the declaration, as for any function nothing defines.

```case
tests/navigation/definition/10_pure_virtual
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Declaration

Navigate from a symbol usage or definition to its declaration. In C++, many entities have separate declarations and definitions.

clice returns the declaration locations plus the definition — symbols defined inline have no separate declaration — minus the site the cursor already stands on, so declaration and definition sites alternate just like go-to-definition. On a function defined with no declaration of its own, it falls back to the functions of its name in its scope that nothing defines: the declaration a changed parameter parted it from.

<!-- BEGIN GENERATED ITEMS: navigation/declaration -->

<!-- BEGIN CAPABILITY: supported -->

**Functions**

Uses and out-of-line definitions navigate to the function prototype

Go-to-declaration reaches a function's prototype both from a call site
and from the out-of-line definition — the two non-cursor sites the
prototype alternates with.

```case
tests/navigation/declaration/01_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Forward-declared record types**

A class with a forward declaration and a later definition offers both from a use — the forward declaration stays part of the declaration set rather than being dropped in favour of the definition

```case
tests/navigation/declaration/02_forward_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static data member**

Static member uses navigate to the in-class declaration

A static data member is declared inside the class and defined out of
line; go-to-declaration on a use offers the in-class declaration
alongside the definition.

```case
tests/navigation/declaration/03_static_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`extern` variable**

External variable uses navigate to their declaration

A use of an `extern` variable offers the `extern` declaration and
the defining declaration together, so the header-side declaration is
always reachable from a use.

```case
tests/navigation/declaration/04_extern_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Multiple declarations**

A use navigates to every declaration site

When a function is declared in several places, go-to-declaration on a use
lists every declaration, not only the nearest one.

```case
tests/navigation/declaration/05_redeclarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Cosmetic signature differences**

Parameter names, and a top-level `const` on a parameter, are not part of a function's type: the declaration and the definition below spell the same function differently, yet go-to-declaration still connects a use to the prototype

```case
tests/navigation/declaration/06_signature_spelling
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**A definition whose declaration changed**

Go-to-declaration on a definition that no longer matches its declaration reaches the declaration it parted from

When a parameter changes on one side only, the definition and the header
declaration become two functions, and the definition has no declaration of
its own. Go-to-declaration on it falls back to the functions of its name and
linkage in its scope that nothing defines: the declaration that wants the
same edit. An overload with a definition of its own is no candidate, nor is a
function of external linkage for a `static` one, and a method defined in
its class never had a declaration apart.

```case
tests/navigation/declaration/07_changed_signatures
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Type Definition

Navigate to the type definition of a symbol. Applicable to variables, parameters, fields, and any other named entity that has a type. A type alias reaches both the alias and the type it names, a pointer-like wrapper both the wrapper and the type it points to; a type's own name, and `this`, reach the type itself.

<!-- BEGIN GENERATED ITEMS: navigation/type_definition -->

<!-- BEGIN CAPABILITY: supported -->

**Variables and parameters**

Go-to-type-definition on a local variable or a parameter reaches the definition of its type

```case
tests/navigation/type_definition/01_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class and struct fields**

Go-to-type-definition on a field access reaches the definition of the field's type

```case
tests/navigation/type_definition/02_fields
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto`-deduced variables**

Go-to-type-definition on a variable declared `auto` reaches its deduced type, as on the `auto` keyword itself

```case
tests/navigation/type_definition/03_deduced_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1026 -->

**Smart-pointer pointee navigation**

Go-to-type-definition on a smart-pointer variable reaches both the wrapper type and the type it points to

```case
tests/navigation/type_definition/04_smart_pointers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

Go-to-type-definition on a variable of an aliased type reaches both the `using` or `typedef` declaration and the type it names

An alias template's alias reaches the template its target specializes.

```case
tests/navigation/type_definition/05_type_aliases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured binding variables**

Go-to-type-definition on a structured binding reaches the definition of the bound member's type

```case
tests/navigation/type_definition/06_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pointers, references and arrays**

Go-to-type-definition looks through pointers, references and arrays to the definition of the element type

```case
tests/navigation/type_definition/07_pointers_and_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this`**

Go-to-type-definition on `this` reaches the class it points to

```case
tests/navigation/type_definition/11_this
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Go to Implementation

From a virtual method, go to implementation lists its overrides at every level below it; from a class, every class deriving from it, directly or not. Go to definition on the declaration of a pure virtual function, which nothing defines, lists the overrides too.

<!-- BEGIN GENERATED ITEMS: navigation/implementation -->

<!-- BEGIN CAPABILITY: supported -->

**Override chain**

Implementation navigation lists the overrides at every level below

Along a three-level override chain, go-to-implementation from the base
reaches the middle and the leaf override, and from the middle the leaf.

```case
tests/navigation/implementation/01_override_chain
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Sibling overrides**

Implementation navigation lists every sibling override

Go-to-implementation on a virtual method lists every override across
the sibling derived classes.

```case
tests/navigation/implementation/02_sibling_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#854 -->

**Functions nothing overrides**

A function nothing overrides is implemented by its definition

Go-to-implementation on the declaration of a function nothing overrides,
virtual or not, reaches its out-of-line definition. On the definition
itself there is nowhere further to go.

```case
tests/navigation/implementation/03_non_virtual
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Base class**

Base classes navigate to every derived class

Go-to-implementation on a base class name lists the classes that derive
from it, directly or through another derived class.

```case
tests/navigation/implementation/04_derived_classes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template duck-type navigation**

Go to implementation on a dependent call reaches the method it resolves to in the template's only instantiation

This applies to function templates and generic lambdas alike, and to a
conversion operator called by name.

```case
tests/navigation/implementation/05_dependent_calls
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Find References

Find references lists every place that names the symbol, its declarations included. A method's references include the calls made through the methods it overrides, which may dispatch to it; a class's the names of its constructors.

<!-- BEGIN GENERATED ITEMS: navigation/references -->

<!-- BEGIN CAPABILITY: supported -->

**Declarations among references**

A reference query returns the declaration and the out-of-line definition together with every use, so the whole surface of a symbol is reachable from any one of its sites

```case
tests/navigation/references/01_declarations_included
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1081 -->

**Range-for references**

Find references on `begin` includes the range-based for loops that call it

A range-based for calls the range's `begin()` and `end()` without writing
their names; its `:` stands for those calls among the references.

```case
tests/navigation/references/02_range_for
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Implicit constructor and destructor calls**

Find references on a constructor or a destructor lists the places that name it, not the objects that run it implicitly

`Blob b;` runs the constructor, and the end of its scope the destructor, but
names neither: listing every object that does would bury the references of
any class with local variables.

```case
tests/navigation/references/03_implicit_construction
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#716 clangd#1872 -->

**References through forwarding functions**

Find references on a constructor does not include call sites that reach it indirectly through a perfect-forwarding factory

```case
tests/navigation/references/04_forwarding_factories
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#258 clangd#675 -->

**Dependent template references**

Find references on a member includes its dependent call sites in an open file's template instantiated only with the member's class

Files that are not open contribute no such call sites.

```case
tests/navigation/references/05_dependent_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2139 -->

**Read/write classification of references**

The reference reply carries only locations, so a reader cannot tell a write from a read; annotating each result with its access kind is not offered

```case
tests/navigation/references/06_access_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#177 -->

**Reference enclosing context**

Each reference reaches the editor as a bare location, without the function that encloses it

clangd attaches the enclosing function through an extension field
(`containerName`) for a client that declares support for it; clice does not
read that declaration yet. `clice query references` names the function each
reference lies in.

```case
tests/navigation/references/07_enclosing_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Label and goto references**

Find references on a label lists the label itself together with every `goto` that jumps to it

```case
tests/navigation/references/08_labels
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1558 -->

**Calls through overridden methods**

Find references on an override includes the calls made through the methods it overrides, which may dispatch to it

`shape.area()` on a `Shape&` may run `Circle::area`, so it is among the
references of the override. A call through `Circle` never runs
`Shape::area`, so it is not among the references of the method overridden;
nor is `Shape::area()` in the override's own body, which names the method
overridden.

```case
tests/navigation/references/13_override_calls
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Deduced Types

On `auto` and `decltype`, navigation reaches the type they stand for.

<!-- BEGIN GENERATED ITEMS: navigation/deduced_types -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced `auto` type navigation**

Go-to-definition on `auto` reaches the type it was deduced to, as if the type were written in its place

```case
tests/navigation/deduced_types/01_auto_keyword
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2055 -->

**`auto` behind pointers and references**

Pointers, references and arrays around the deduced type are stripped: `auto*`, `const auto&`, `auto&&`, `decltype(auto)` and an `auto` deduced as a pointer all reach the class

The `auto` of a structured binding reaches the type of the object it
decomposes.

```case
tests/navigation/deduced_types/02_auto_declarators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` type navigation**

Go-to-definition on `decltype` reaches the type its operand names, through any `decltype` that type was itself declared with

```case
tests/navigation/deduced_types/03_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced templates and aliases**

An `auto` deduced as a template specialization reaches the template, or the explicit or partial specialization it selects; one deduced through an alias reaches the alias

A builtin type or a lambda's closure type has no declaration to reach, and
the `auto` of a `new` expression navigates nowhere. A macro spelling `auto`
navigates to the macro.

```case
tests/navigation/deduced_types/04_deduced_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` parameters and return types**

The `auto` of an abbreviated template's parameter reaches the type of its only instantiation, and an `auto` return type the type returned

A parameter whose template is instantiated with several types reaches
none of them. The leading `auto` of a trailing return type reaches the
type after the arrow.

```case
tests/navigation/deduced_types/05_parameters_and_returns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` in templates**

Inside a template, an `auto` whose initializer depends on a template parameter reaches the type the initializer resolves to

A template parameter, whether of a function template or a class template,
stands for its argument when the template is instantiated once, and for
itself when it is instantiated with several types.

```case
tests/navigation/deduced_types/06_dependent_auto
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Library-internal aliases**

An `auto` deduced through a library's internal alias reaches the type the alias names; one deduced through a public alias reaches the alias

A name a library reserves for itself, `std::__detail::__unique_ptr_t`, is
not one the code would write, so `auto` sees through it to `unique_ptr`. A
public alias such as `iterator` is the name the library gives its users.

```case
tests/navigation/deduced_types/08_library_aliases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Templates

Inside a template, a name that depends on its parameters still navigates to the declarations it can reach.

<!-- BEGIN GENERATED ITEMS: navigation/templates -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent member navigation**

Inside a template that is never instantiated, a member accessed on an object of a dependent type resolves to the member declared on the corresponding class template

```case
tests/navigation/templates/01_dependent_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#212 -->

**Template specialization navigation**

Go-to-definition on the name of an explicit specialization reaches the primary template it specializes

```case
tests/navigation/templates/02_explicit_specializations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent overload candidates**

A dependent call that may reach several overloads lists each of them

Every candidate answers on its own: the definition where it has one, its
declaration where it has none.

```case
tests/navigation/templates/03_overload_candidates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent expression members**

A member accessed on what a dependent subscript, call or `auto` variable evaluates to resolves to the member declared on the class template

Where the call has a `const` overload, the constness of the object picks
the one it names; overloads that differ in their parameters are all
listed.

```case
tests/navigation/templates/04_dependent_expressions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

Names written in macro arguments or created by macros navigate like any other, and a macro's references include each place that expands or tests it.

<!-- BEGIN GENERATED ITEMS: navigation/macros -->

<!-- BEGIN CAPABILITY: supported -->

**Names in macro arguments**

A name spelled in a macro argument navigates like a name written outside a macro

Definition and declaration alternate at its spelling, and a later use
reaches the function the macro declares. The macro's own name, at its
definition or an invocation, resolves to the macro.

```case
tests/navigation/macros/01_macro_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-generated names**

A name a macro creates resolves to the invocation that creates it

A use of a name assembled by token paste, or spelled in the macro's body,
reaches the invocation, and the invocation's own name resolves to the
macro. A token inside the macro's body has no meaning until an expansion
gives it one, so navigation on it answers nothing.

```case
tests/navigation/macros/02_generated_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro references**

Macro reference searches include expansions, conditional tests and undefinitions

Each `#define` of a name is its own symbol, so a redefinition after
`#undef` collects only its own uses.

```case
tests/navigation/macros/04_macro_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#346 -->

**Nested macro references**

Find references on a macro does not include the mentions of it written inside the bodies of other macro definitions

```case
tests/navigation/macros/05_nested_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Header macros defined per unit**

Go-to-definition on a macro a shared header uses reaches every definition the units including it expand

Two units include `config.h`, one with `FAST` defined: the header's `MODE`
expands to a different definition in each, and from a closed header
go-to-definition lists both.

```case
tests/navigation/macros/10_header_variants
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Implicit targets

Navigate to definitions of implicitly invoked code. In C++ many constructs generate hidden calls to constructors, operators, conversions, etc. Navigating from the syntactic construct (a brace, a keyword, an operator token) to the actual function being called is essential for understanding what code is really executing.

Implicit navigation requires an unambiguous source token — patterns where the token already has a well-defined go-to-def target (e.g., a variable name always goes to its declaration) cannot be repurposed for implicit call navigation.

<!-- BEGIN GENERATED ITEMS: navigation/implicit_targets -->

<!-- BEGIN CAPABILITY: supported -->

**`override` / `final`**

`override` and `final` navigate to the base method they override

Go-to-definition on the `override` or `final` specifier reaches the base
class virtual method it overrides.

```case
tests/navigation/implicit_targets/01_override_and_final
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**`break` / `continue`**

`break` and `continue` do not navigate to their enclosing control statement yet

Go-to-definition on `break` or `continue` does not reach the head of the
loop or switch it controls.

```case
tests/navigation/implicit_targets/02_break_and_continue
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`delete` expression**

`delete` navigates to the destructor and the deallocation function it runs

Go-to-definition on `delete` reaches the destructor it runs and the class's
own `operator delete`.

```case
tests/navigation/implicit_targets/03_delete_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`new` expression**

`new` navigates to the allocation function and the constructor it runs

Go-to-definition on `new` reaches the class's overloaded `operator new` and
the constructor the same expression invokes.

```case
tests/navigation/implicit_targets/04_new_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Overloaded operators**

An overloaded operator token navigates to its definition

Go-to-definition on an overloaded operator token reaches the operator's
definition. The binary, subscript, call and arrow operators (`+`, `[]`,
`()`, `->`) are all resolved.

```case
tests/navigation/implicit_targets/05_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C++20 rewritten operators**

Rewritten comparisons navigate to the operator that implements them

For a comparison synthesized by the C++20 rewrite rules, go-to-definition
on the written operator reaches the operator that actually implements it:
`!=` reaches `operator==`, and `>` reaches `operator<=>`.

```case
tests/navigation/implicit_targets/06_rewritten_comparisons
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**User-defined literals**

A literal suffix navigates to its literal operator

Go-to-definition on the suffix of a user-defined literal reaches the
`operator""` it calls.

```case
tests/navigation/implicit_targets/07_user_defined_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1931 -->

**Implicit conversion operators**

A condition or `!` navigates to the conversion operator it runs

Go-to-definition on the parenthesis of a condition (the `while` of a
`do`), or on the `!`, `&&`, `||` or `?` whose operand converts, reaches the
user-defined conversion operator that tests the operand.

```case
tests/navigation/implicit_targets/08_implicit_conversions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Cast conversion navigation**

A cast navigates to the constructor or conversion operator it runs

Go-to-definition on a `static_cast`, on the parenthesis of a C-style cast
or on the type of a functional cast reaches the constructor it selects or
the user-defined conversion operator it calls.

```case
tests/navigation/implicit_targets/09_casts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Range-based for**

The range-for colon navigates to `begin()` and `end()`

Go-to-definition on the `:` of a range-based for reaches the `begin()` and
`end()` chosen for the range.

```case
tests/navigation/implicit_targets/10_range_for
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`co_await` / `co_yield` / `co_return`**

Coroutine keywords navigate to the methods they call

Go-to-definition on `co_await` reaches the awaiter's `await_ready`,
`await_suspend` and `await_resume`; on `co_yield`, the promise's
`yield_value`; on `co_return`, the promise's `return_value`.

```case
tests/navigation/implicit_targets/11_coroutine_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Implicit Construction

Navigation from initialization, return, capture and decomposition syntax reaches
constructors, aggregate definitions or bindings selected implicitly.

<!-- BEGIN GENERATED ITEMS: navigation/implicit_construction -->

<!-- BEGIN CAPABILITY: supported -->

**Constructor calls**

Parentheses and braces navigate to the selected constructor

Go-to-definition on the opening parenthesis or brace of a constructor
call reaches the constructor overload resolution selected, for both the
`T(args)` and `T{args}` forms.

```case
tests/navigation/implicit_construction/01_constructor_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Copy/move construction and assignment**

The `=` of an initialization or an assignment navigates to the function it runs

Go-to-definition on the `=` of an assignment reaches the assignment
operator, and on the `=` of a copy- or move-initialization (`T b = a;`) the
copy or move constructor that builds the variable.

```case
tests/navigation/implicit_construction/02_copy_and_move
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**CTAD**

A CTAD construction navigates to the deduced specialization's constructor

When class template argument deduction picks a specialization, go-to-
definition on the constructor call reaches the constructor that was
selected, not merely the class template.

```case
tests/navigation/implicit_construction/03_deduced_class_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Aggregate initialization**

Aggregate initializer braces navigate to the aggregate definition

An aggregate has no constructor, so go-to-definition on its initializer
brace reaches the aggregate's definition.

```case
tests/navigation/implicit_construction/04_aggregates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Member initializer list**

Member-initializer parentheses navigate to the selected base or member constructor

The base and member constructors run by an initializer list are reached
from the opening parenthesis of each initializer. The initializer name
itself resolves to the base type or the member, so navigation to the
constructor goes through the parenthesis.

```case
tests/navigation/implicit_construction/05_member_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Delegating constructors**

Delegating-constructor parentheses navigate to the target constructor

A delegating constructor's target is reached from the opening parenthesis
of the delegated call. The constructor name itself resolves to the class
type, so navigation to the target constructor goes through the
parenthesis.

```case
tests/navigation/implicit_construction/06_delegating_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inherited constructors**

An inherited-constructor declaration navigates to every imported base constructor

Go-to-definition on an inherited-constructor declaration
(`using Base::Base;`) lists each constructor of the base it imports.

```case
tests/navigation/implicit_construction/07_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Return value implicit construction**

A braced return value navigates to the selected constructor

A braced `return {args}` implicitly constructs the function's return
type; go-to-definition on the brace reaches the selected constructor.

```case
tests/navigation/implicit_construction/08_return_values
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda init-capture**

A lambda init-capture navigates to the constructor building the capture

Go-to-definition on the `=` of a lambda init-capture reaches the
constructor that builds the captured value.

```case
tests/navigation/implicit_construction/09_lambda_captures
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

A structured binding's name is its own definition

Go-to-definition on the name a structured binding declares stays on the
binding, as on any declared name; the member or accessor it binds to is
not a definition of it.

```case
tests/navigation/implicit_construction/10_structured_bindings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Navigation

Module names in `import` and `module` declarations navigate to the interface unit that declares the module.

<!-- BEGIN GENERATED ITEMS: navigation/modules -->

<!-- BEGIN CAPABILITY: supported clangd#2310 -->

**Module import navigation**

Go-to-definition on the name in an `import` declaration opens the module interface unit that exports it, and uses of an imported symbol reach its definition in that unit

```case
tests/navigation/modules/01_import_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module implementation units**

The module names of an interface and its implementation units navigate to each other

The module name in an implementation unit reaches the interface unit
declaring the module, and the interface's module name the units
implementing it. A use of an exported function reaches its definition in
the implementation unit.

```case
tests/navigation/modules/02_implementation_units
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dot-separated module name**

Every segment of a dotted module name navigates to its interface

Go-to-definition on any segment of a dot-separated module name reaches
the module's interface unit; the whole name is one reference.

```case
tests/navigation/modules/03_dotted_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Across a Project

Navigation follows a symbol into the other files of the project: a declaration in a header, a definition in a source, a module partition.

<!-- BEGIN GENERATED ITEMS: navigation/projects -->

<!-- BEGIN CAPABILITY: supported -->

**Header declarations**

A call names the function's definition in its source file and its declaration in the header

```case
tests/navigation/projects/01_header_to_source
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module partitions**

A call into a module lands on the partition exporting the function

```case
tests/navigation/projects/02_module_partition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Uses across sources**

Find references gathers the uses in every source of the project, not only in the current file

A function defined in one source and called from a sibling lists both call
sites, together with its declaration in the shared header.

```case
tests/navigation/projects/03_uses_across_sources
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Call Hierarchy

A call hierarchy lists the callers of a function and the functions it calls; a call made through a method a function overrides counts among its callers. Each item shows the function's parameters, which tell overloads apart. A function's local variables and parameters get no item: their only caller is the function itself.

<!-- BEGIN GENERATED ITEMS: navigation/call_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Functions and methods**

A call hierarchy opens on a free function and on a member method alike

```case
tests/navigation/call_hierarchy/01_functions_and_methods
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Incoming calls**

Incoming calls list every caller of a function, and a caller that invokes it more than once contributes each call site

```case
tests/navigation/call_hierarchy/02_incoming_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Outgoing calls**

Outgoing calls list every function a body invokes, one entry per callee

```case
tests/navigation/call_hierarchy/03_outgoing_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call hierarchy item details**

A call hierarchy item shows the function's parameters beside its name, telling overloads apart

```case
tests/navigation/call_hierarchy/04_signature_details
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Qualified name for member functions**

A member function's call hierarchy item shows its qualified name (`Circle::draw`) beside its name, telling it apart from a free function

```case
tests/navigation/call_hierarchy/05_qualified_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Calls through an override**

A call through a virtual method reaches every override it may dispatch to, a qualified call only the method it names

`b.draw()` on a `Base&` may run `Base::draw` or `Derived::draw`, so both
list it among their incoming calls. `d.draw()` on a `Derived&` runs
`Derived::draw` or an override below it, never `Base::draw`, so only the
override lists it. `Base::draw()`, in an override's body or anywhere else,
runs `Base::draw` alone, while `Group`'s `child->draw()` may reach either.

```case
tests/navigation/call_hierarchy/06_virtual_dispatch
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1308 -->

**Non-function targets**

A call hierarchy opens on a variable, a field or an enum constant

Preparing a call hierarchy on a variable, a field or an enum constant
offers an item whose incoming calls are the functions using it.

```case
tests/navigation/call_hierarchy/07_non_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Calls inside lambdas**

A call written in a lambda body appears in the incoming calls of the function it invokes, attributed to the function that encloses the lambda

```case
tests/navigation/call_hierarchy/08_lambda_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2242 -->

**Constructor calls through forwarding functions**

Incoming calls of a constructor do not include the call sites that reach it through a perfect-forwarding factory

```case
tests/navigation/call_hierarchy/09_forwarding_factories
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function locals**

A function's local variables and parameters get no call hierarchy item

The only function that can use them is the one declaring them, so an item
for them would have one caller, known in advance. A field of a class
declared in a function keeps its item: the class's methods use it.

```case
tests/navigation/call_hierarchy/15_function_locals
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Hierarchy

A type hierarchy lists the bases of a class and the classes deriving from it.

<!-- BEGIN GENERATED ITEMS: navigation/type_hierarchy -->

<!-- BEGIN CAPABILITY: supported -->

**Type kinds**

A type hierarchy opens on any user-defined type: class, struct, enum and union alike

```case
tests/navigation/type_hierarchy/01_type_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Supertypes**

Supertypes list every direct base of a class, including each base of a multiple-inheritance derived type

A base written through an alias lists the class the alias names.

```case
tests/navigation/type_hierarchy/02_supertypes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Subtypes**

Subtypes list every class that derives from a base, across sibling derived types

```case
tests/navigation/type_hierarchy/03_subtypes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template inheritance**

Subtypes of a base include classes that derive from it through a class template, such as a CRTP wrapper

```case
tests/navigation/type_hierarchy/04_template_bases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Document Highlight

Highlight all references to the symbol under cursor within the current file (`textDocument/documentHighlight`). Highlights come from the same index as Find References, so they cover the same names and work in read-only mode too; a variable, a field or a parameter is marked written or read where the code writes or reads it; every other highlight is plain.

### Declarations and Uses

Every name of the symbol in the file is highlighted, macros included.

<!-- BEGIN GENERATED ITEMS: document_highlight/symbols -->

<!-- BEGIN CAPABILITY: supported -->

**Declarations and uses**

Every name of the symbol under the cursor in the file is highlighted, its declarations and definition included

```case
tests/document_highlight/symbols/01_declarations_and_uses
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro names and arguments**

A macro highlights at its definition, its expansions, the conditionals testing it and its `#undef`

A name written in a macro argument highlights where it is written; a name the macro's replacement spells highlights the whole invocation. A macro used in another macro's replacement highlights neither there nor at that macro's invocations.

```case
tests/document_highlight/symbols/03_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1921 -->

**Control flow keywords**

A `break` or `continue` does not highlight the loop it leaves yet

```case
tests/document_highlight/symbols/08_control_flow
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Reads and Writes

Each highlight tells whether the code there reads the symbol or writes it.

<!-- BEGIN GENERATED ITEMS: document_highlight/access_kinds -->

<!-- BEGIN CAPABILITY: supported -->

**Reads and writes**

Every use of a variable is highlighted, assignments as writes and other uses as reads

```case
tests/document_highlight/access_kinds/01_reads_and_writes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Arguments by mutable reference**

An argument bound to a non-const reference parameter is a write, while a const reference, a copy or taking its address reads it

```case
tests/document_highlight/access_kinds/03_reference_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Member access**

Writing through `object.member` writes the member and reads the object

A designated initializer or a constructor's member initializer names the field without writing it. Fields of an anonymous union highlight like any other field.

```case
tests/document_highlight/access_kinds/06_member_access
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Module Units

Highlights work in module units and on the names a module exports.

<!-- BEGIN GENERATED ITEMS: document_highlight/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module units**

Highlights work in module units and on names imported from a module, the module name included

```case
tests/document_highlight/modules/01_module_units
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Workspace Symbol

Search the whole project for a symbol by name (`workspace/symbol`), the names of internal linkage at namespace scope (`static`, an anonymous namespace) included. Each result has the kind the outline gives it, and a function its parameters.

### Queries

A query matches names by their words, and its syntax narrows the search.

<!-- BEGIN GENERATED ITEMS: workspace_symbol/queries -->

<!-- BEGIN CAPABILITY: supported -->

**Search by name**

A query finds the symbols whose names match it, members with their container

```case
tests/workspace_symbol/queries/01_name_search
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#914 -->

**Fuzzy matching**

A query matches a name as a subsequence aligned to its words

`LinLis` finds `LinkedList` and `pconf` finds `parse_config`: after its
first letter, every letter of the query either continues a run or starts
a word of the name, so `pcfg` finds nothing — its `f` lands in the middle
of `config`.

```case
tests/workspace_symbol/queries/03_fuzzy_matching
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Query syntax**

Quotes, wildcards, scopes and filters narrow a search

`"process"` matches the whole name only and `proc*` whatever it globs;
`io::*` lists a namespace's members and `io::**` its whole subtree;
`kind:function` keeps one kind. Terms combine, separated by spaces.

```case
tests/workspace_symbol/queries/04_query_syntax
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Search by mangled (linker) name**

Mangled linker names do not resolve to their source functions yet

```case
tests/workspace_symbol/queries/05_mangled_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Qualified Names

A qualified query names the scopes the symbol must lie in.

<!-- BEGIN GENERATED ITEMS: workspace_symbol/scopes -->

<!-- BEGIN CAPABILITY: supported clangd#550 -->

**Qualified name search**

A qualified query names the containers the symbol must lie in

`net::Socket` finds `deep::net::Socket`: the qualifiers must appear in the
symbol's container chain in that order, with other containers allowed
around them, while a leading `::` demands exactly that chain. Replies to
a qualified query spell the qualified name, so editors that filter
results against the query text keep them.

```case
tests/workspace_symbol/scopes/01_qualified_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#931 -->

**Scoped enumerator lookup**

An enum qualifies its enumerators like any other container

```case
tests/workspace_symbol/scopes/02_enumerators
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Ranking

The name spelled exactly ranks first.

<!-- BEGIN GENERATED ITEMS: workspace_symbol/ranking -->

<!-- BEGIN CAPABILITY: supported clangd#2253 -->

**Alias ranking**

The name spelled exactly ranks above the names merely starting with it

`Connection` lists the alias first and `ConnectionImpl` after it.

```case
tests/workspace_symbol/ranking/01_exact_name
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Results

Each result names its symbol and the container it lies in.

<!-- BEGIN GENERATED ITEMS: workspace_symbol/results -->

<!-- BEGIN CAPABILITY: supported clangd#1344 -->

**Overload disambiguation**

Workspace symbol results show each function's parameters, telling overloads apart

Querying an overloaded name finds every overload, each named with its
parameters: the two `process` results differ at a glance.

```case
tests/workspace_symbol/results/01_overloads
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Names of internal linkage**

Search finds the names of internal linkage at namespace scope, in every file declaring one, but never a function's locals

A `static` variable or a member of an anonymous namespace is known only to
its own file, yet it is a name the project declares: search lists it, once
per file that declares one. A local variable is not.

```case
tests/workspace_symbol/results/06_internal_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Files Not Open

The search spans every file of the project, open in the editor or not.

<!-- BEGIN GENERATED ITEMS: workspace_symbol/projects -->

<!-- BEGIN CAPABILITY: supported -->

**Search spans the whole project**

Workspace symbol search returns hits from project files that are not open

The query finds a function in `other.cpp`, which stays closed while
`main.cpp` is open: the background index answers for it.

```case
tests/workspace_symbol/projects/01_closed_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Copies of one checkout**

Two checkouts of the same code opened side by side list each symbol once, from the folder being edited

Two worktrees of one repository opened as workspace folders hold the same
symbols at the same places in the same text. Search lists each once, the
copy of the folder with an open document: the one the editor works in. A
folder whose file reads otherwise holds a symbol of its own, listed too.

```case
tests/workspace_symbol/projects/03_worktree_copies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Switch Source/Header

Open the file the current one pairs with: a header's source, a source's header, a module interface's implementation units, an implementation unit's interface. In VS Code it is **Switch Source/Header** in the editor's right-click menu, `Alt+O`, or **Clice: Switch Source/Header** in the Command Palette; Neovim runs it as `:LspCliceSwitchSourceHeader`, and `clice query counterparts <file>` answers the same on the command line. Zed lets no extension add a command, and its own _switch source header_ works with clangd only.

The candidates come from what the build and the index already know, so nothing is compiled for the answer: the files of the same name on the other side — in the file's own directory, or elsewhere in the workspace when one includes the other —, the declarations one holds that the other defines, and module declarations. Each candidate carries its reasons. The editor opens the best one directly when it is the only candidate or beats every other: it has each of the other's reasons among shared declarations and the same name, at least as many shared declarations, and either a reason more, twice the shared declarations when both share some, or — neither sharing any — a module pairing the other lacks. Otherwise it lists the candidates to pick from.

### Headers and Sources

A header pairs with the source that defines its declarations, and the source with the header.

<!-- BEGIN GENERATED ITEMS: switch_source_header/counterparts -->

<!-- BEGIN CAPABILITY: supported -->

**Header and source**

A header pairs with the source defining its declarations, and the source with the header

```case
tests/switch_source_header/counterparts/01_header_and_source
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Template definitions apart**

A header and the `.tpp` file it includes for its template definitions switch to each other

```case
tests/switch_source_header/counterparts/02_template_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Files without a counterpart**

A header that defines everything it declares, and a `.def` fragment, have no counterpart

```case
tests/switch_source_header/counterparts/03_no_counterpart
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Directory Layouts

A header finds its source across directories, such as `include/` and `src/`.

<!-- BEGIN GENERATED ITEMS: switch_source_header/layouts -->

<!-- BEGIN CAPABILITY: supported -->

**Headers apart from sources**

A header under `include/` finds its source under `src/`, ahead of another file of the same name that defines none of its declarations

Away from the header's own directory, a file of the same name counts
only when one of the two includes the other — `tools/shape.cpp` includes
nothing and is no counterpart — and of those only the nearest:
`src/legacy/shape.cpp` lies a directory deeper than `src/shape.cpp`.

```case
tests/switch_source_header/layouts/01_split_directories
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Several Candidates

When several files could pair, the clearest one opens directly and the others stay listed.

<!-- BEGIN GENERATED ITEMS: switch_source_header/ranking -->

<!-- BEGIN CAPABILITY: supported -->

**Definitions across several sources**

When several sources define a header's declarations, the one defining clearly the most is taken directly and the others stay listed

```case
tests/switch_source_header/ranking/01_scattered_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**No clear counterpart**

When no candidate clearly outweighs the others, the editor lists them with their reasons to pick from

```case
tests/switch_source_header/ranking/02_split_evenly
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Module Interfaces

A module interface unit pairs with the units implementing its module.

<!-- BEGIN GENERATED ITEMS: switch_source_header/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module interface and implementations**

A module interface unit pairs with the units implementing its module, and each of them with the interface

```case
tests/switch_source_header/modules/01_module_units
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Partition beside its implementation**

An internal partition and the implementation unit of the same name defining its declarations switch to each other directly

The implementation unit also lists the primary interface of its module,
after the partition.

```case
tests/switch_source_header/modules/02_partitions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
