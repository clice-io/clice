# Inlay Hints

<!-- The cards in the sections below are generated from the documented cases
     in tests/inlay_hints/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice renders inline annotations for the information the code leaves implicit: parameter names at call sites, deduced types, the field names behind positional aggregate initialization, and what a closing brace or an `#endif` closes. Every kind of hint has its own switch in the `[inlay_hints]` configuration section; each section below says which are off by default.

## Parameter Hints

A call shows the name of the parameter each argument binds to, unless the argument already says it.

<!-- BEGIN GENERATED ITEMS: inlay_hints/parameter_hints -->

<!-- BEGIN CAPABILITY: supported -->

**Argument names**

A literal argument shows the name of the parameter it binds to

```case
tests/inlay_hints/parameter_hints/01_argument_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1877 -->

**Hint suppression**

Named arguments and `/*name=*/` comments suppress parameter hints

An argument spelled like its parameter, or a bare member of that name, needs no hint; a qualified name or a member reached through an object is no match. A comment naming another parameter suppresses nothing.

```case
tests/inlay_hints/parameter_hints/03_suppression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1123 -->

**Mutable reference markers**

`&` flags arguments passed by non-const lvalue reference

```case
tests/inlay_hints/parameter_hints/05_reference_markers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2324 -->

**Forwarding resolution**

Packs forwarded through wrappers resolve to the target's parameter names

A plain pass-through works without `std::forward`, a pack between fixed leading and trailing arguments resolves to the parameters it lands in, and a pack handed on to a callable object resolves to its call operator.

```case
tests/inlay_hints/parameter_hints/06_forwarding
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Names from definitions**

Unnamed declaration parameters take the definition's names; leading underscores strip

When both the declaration and the definition name a parameter, the declaration wins.

```case
tests/inlay_hints/parameter_hints/07_definition_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1734 clangd#1742 -->

**Function pointers and call operators**

Indirect calls still name their parameters

```case
tests/inlay_hints/parameter_hints/08_function_objects
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent calls**

Parameter names appear even when the callee is only known inside a template

Candidates are matched by argument count; only a unique surviving candidate names the parameters, so a call that could still hit several overloads stays bare rather than guessing.

```case
tests/inlay_hints/parameter_hints/10_dependent_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2620 -->

**Macros at call sites**

Arguments spelled as macros hint; calls generated inside macro bodies do not

An object-like macro is one written argument. A macro covering several arguments has no place to anchor a hint, and code written as a macro argument keeps its hints.

```case
tests/inlay_hints/parameter_hints/12_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2248 -->

**Sloppy name matching**

`aParam` does not yet suppress an argument spelled `param`

```case
tests/inlay_hints/parameter_hints/16_near_matches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1364 -->

**Inherited constructors**

A constructor that `using Base::Base` brings in names its parameters as the base declares them

```case
tests/inlay_hints/parameter_hints/17_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Hints

A declaration whose type is deduced shows that type.

<!-- BEGIN GENERATED ITEMS: inlay_hints/type_hints -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced `auto` variables**

The hint shows the full variable type, qualifiers included

```case
tests/inlay_hints/type_hints/01_auto_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1298 clangd#1357 -->

**Type sugar length limits**

Aliases keep their spelling, and an over-long type falls back to its sugared name

A type without a spelling short enough is dropped, and default template arguments never print. The budget is `inlay_hints.type_name_limit`; `0` lifts it.

```case
tests/inlay_hints/type_hints/02_type_sugar
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

Each binding hints its own type; the aggregate itself stays bare

```case
tests/inlay_hints/type_hints/03_structured_bindings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1163 -->

**Lambdas**

Variables, deduced return types, and init-captures all hint

```case
tests/inlay_hints/type_hints/04_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced return types**

Deduced return types appear as `-> T` after the parameter list

A declaration hints once a later definition supplies the deduction, while one without a definition stays silent. A written trailing return type needs no hint.

```case
tests/inlay_hints/type_hints/05_deduced_return_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` parameters**

A template with exactly one instantiation reveals the deduced type

A second instantiation makes the deduction ambiguous and drops the hint, and a pack and the parameters after it never hint. A template body hints once across instantiations deducing the same type.

```case
tests/inlay_hints/type_hints/07_auto_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1749 -->

**Explicitly spelled initializers**

A type the initializer already spells, in a cast or a functional cast, gets no hint

A class template whose arguments the initializer leaves to deduction is still
hinted.

```case
tests/inlay_hints/type_hints/08_explicit_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scope suppression**

Namespace qualifiers drop from hints; class scopes stay

```case
tests/inlay_hints/type_hints/10_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2275 -->

**Instantiated templates**

An `auto` variable inside a class template shows the type the template's only instantiation gives it

With two instantiations deducing different types there is no one type to show.

```case
tests/inlay_hints/type_hints/12_instantiated_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Binding aliases**

The bindings of a container's elements keep the aliases the container was declared with

Iterating a `std::map<std::string, Point>` shows each key as a
`const std::string`, not as `basic_string<char>`. Two aliases of one type,
or an alias and the type itself, leave it unaliased.

```case
tests/inlay_hints/type_hints/13_binding_aliases
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Designator Hints

Positional aggregate initialization shows the field or the index each value initializes.

<!-- BEGIN GENERATED ITEMS: inlay_hints/designator_hints -->

<!-- BEGIN CAPABILITY: supported clangd#2303 -->

**Field and index designators**

Positional aggregate initialization shows `.field=` and `[index]=`

Array designators survive members whose size depends on a template parameter, and reserved member names are skipped rather than printed.

```case
tests/inlay_hints/designator_hints/01_fields_and_indices
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested aggregates**

Written braces recurse; omitted braces flatten into `.outer.inner=`

```case
tests/inlay_hints/designator_hints/02_nested_aggregates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Designator suppression**

Written designators and `/*name=*/` comments keep their inits bare

Mixing written designators with positional initializers is a C99 extension clang accepts with a warning; only the bare `4` needs a hint.

```case
tests/inlay_hints/designator_hints/04_suppression
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Aggregates only**

Constructor calls, copies and idiomatic zero-init produce no designators

A braced constructor call names the constructor's parameters instead, and the idiomatic `{}` zero-initializer stays quiet.

```case
tests/inlay_hints/designator_hints/05_aggregates_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2540 -->

**Parenthesized aggregate initialization**

C++20 `Point(1, 2)` shows the designators braces would, `.x=` and `.y=`

```case
tests/inlay_hints/designator_hints/07_parenthesized_init
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Block End Hints

The end of a block ten lines or taller names what it closes: a namespace and an `#endif` by default, functions, types and control flow once `inlay_hints.block_end` turns them on.

<!-- BEGIN GENERATED ITEMS: inlay_hints/block_end -->

<!-- BEGIN CAPABILITY: supported -->

**Names after closing braces**

After the closing brace of a block spanning lines, a hint names what the brace closes

Functions, types, namespaces and control flow get one; `if` and `while` show their condition when it is short, an `else if` chain shows as plain `// if`, and a label longer than 60 characters is dropped. A block on one line gets none.

```case
tests/inlay_hints/block_end/01_block_end_hints
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Default Argument Hints

Off by default (`inlay_hints.default_arguments`). Call sites that rely on default arguments show what was omitted, abbreviated past `inlay_hints.max_length.default_arguments`.

<!-- BEGIN GENERATED ITEMS: inlay_hints/default_arguments -->

<!-- BEGIN CAPABILITY: supported -->

**Omitted default arguments**

A call relying on default arguments shows what it omitted, each name linking to its parameter

```case
tests/inlay_hints/default_arguments/01_omitted_arguments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Clickable Hints

The names in a hint are links to the declarations of what they name, and the name after a closing brace or an `#endif` to where its block starts.

<!-- BEGIN GENERATED ITEMS: inlay_hints/links -->

<!-- BEGIN CAPABILITY: supported clangd#1535 -->

**Clickable type names**

Each type name in a type hint links to its declaration: clicking it goes to the definition, hovering it shows the type's card

A class declared ahead of its definition links to that declaration, from which go-to-definition reaches the definition. Template arguments link one by one; builtin types and punctuation stay plain text.

```case
tests/inlay_hints/links/02_type_links
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Hint Kinds

The template arguments class template argument deduction found.

<!-- BEGIN GENERATED ITEMS: inlay_hints/other_kinds -->

<!-- BEGIN CAPABILITY: unsupported clangd#2583 -->

**Template argument names**

An explicit template argument at a call site does not show the template parameter it binds yet

```case
tests/inlay_hints/other_kinds/01_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2331 -->

**Deduced class template arguments**

A class template whose arguments a constructor call deduces does not show them yet

```case
tests/inlay_hints/other_kinds/02_ctad_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2254 -->

**Implicit conversions**

An argument converted implicitly at a call site shows no conversion yet

```case
tests/inlay_hints/other_kinds/03_implicit_conversions
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Configuration

The `[inlay_hints]` section of `clice.toml` (or the same keys via `initializationOptions`) holds a switch for every kind of hint, the height from which a block's end gets a hint, and the length limits under `[inlay_hints.max_length]`. See the [configuration guide](../guide/configuration.md#inlay-hints) for details. Configuration changes take effect after a server restart — a recompile is never involved.

## Interactive Behavior

- Requests are range-scoped: hints outside the requested range are discarded.
- Parameter hints anchor to the left of their argument; type and designator hints anchor to their declaration side with LSP padding flags instead of embedded spaces.
- Identical duplicate hints (e.g. from template instantiations) collapse into one.
- Type names, parameter names and designated fields in a hint are links: clicking one goes to the definition of what it names, and hovering it shows that symbol's card. A link points at a declaration of the symbol when one exists, so that go-to-definition from it reaches the definition. The links come with the hints; clients without LSP 3.17 inlay hint support receive plain text.

## Other Known Gaps

- [ ] Abbreviated type hints with expandable label parts via `InlayHintLabelPart` ([clangd#2269](https://github.com/clangd/clangd/issues/2269))
