# Inlay Hints

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/inlay_hint/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice renders inline annotations for the information the code leaves implicit: parameter names at call sites, deduced types, and the field names behind positional aggregate initialization. Hint categories can be toggled individually through the `[inlay_hints]` configuration section; the sections below describe the categories that are on by default.

## Parameter Hints

<!-- BEGIN GENERATED ITEMS: parameter_hints -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter name hints**

Call sites show parameter names for functions and constructors

```snap
tests/snap/inlay_hint/parameter_hints/01_param_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1877 -->

**Hint suppression**

Named arguments and `/*name=*/` comments suppress parameter hints

```snap
tests/snap/inlay_hint/parameter_hints/02_param_suppression.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Setter and builtin suppression**

`setX(x)` and `std::move`/`std::forward` arguments stay bare

```snap
tests/snap/inlay_hint/parameter_hints/03_param_setters_builtins.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1123 -->

**Mutable reference markers**

`&` flags arguments passed by non-const lvalue reference

```snap
tests/snap/inlay_hint/parameter_hints/04_param_references.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2324 -->

**Forwarding resolution**

Packs forwarded through wrappers resolve to the target's parameter names

```snap
tests/snap/inlay_hint/parameter_hints/05_param_forwarding.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Names from definitions**

Unnamed declaration parameters take the definition's names; leading
underscores strip

```snap
tests/snap/inlay_hint/parameter_hints/06_param_definition_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1734 clangd#1742 -->

**Function pointers and call operators**

Indirect calls still name their parameters

```snap
tests/snap/inlay_hint/parameter_hints/07_param_function_objects.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1777 -->

**Deducing `this`**

The explicit object parameter never hints (C++23)

```snap
tests/snap/inlay_hint/parameter_hints/08_param_deducing_this.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Dependent calls**

Parameter names appear even when the callee is only known inside a template

Candidates are matched by argument count; only a unique surviving
candidate names the parameters, so a call that could still hit several
overloads stays bare rather than guessing.

```snap
tests/snap/inlay_hint/parameter_hints/09_param_dependent.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unexpanded packs**

A written pack expansion breaks the 1:1 argument mapping and stops hinting

```snap
tests/snap/inlay_hint/parameter_hints/10_param_packs.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2620 -->

**Macros at call sites**

Arguments spelled as macros hint; calls generated inside macro bodies do not

```snap
tests/snap/inlay_hint/parameter_hints/11_param_macros.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Implicit constructor calls**

Conversions the code never wrote produce no hints of their own

```snap
tests/snap/inlay_hint/parameter_hints/12_param_implicit_conversions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pseudo-object expressions**

MS property accesses stay quiet; written subscripts keep the accessor's
names

```snap
tests/snap/inlay_hint/parameter_hints/13_param_pseudo_objects.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1034 -->

**Explicit instantiation**

An explicit instantiation definition adds no duplicate hints, while its
written template arguments hint normally

```snap
tests/snap/inlay_hint/parameter_hints/14_param_explicit_instantiation.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2248 -->

**Sloppy name matching**

`aParam` does not yet suppress an argument spelled `param`

```snap
tests/snap/inlay_hint/parameter_hints/15_param_case_insensitive.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1364 -->

**Inherited constructors**

`using Base::Base` calls lose their parameter names

```snap
tests/snap/inlay_hint/parameter_hints/16_param_inherited_constructors.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous parameters**

Unnamed parameters produce no name hint, though mutable references still
show `&`

```snap
tests/snap/inlay_hint/parameter_hints/17_param_anonymous.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Operators and literals**

Operator syntax and user-defined literals stay bare; member and default
member initializers hint

```snap
tests/snap/inlay_hint/parameter_hints/18_param_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Packs in constructor arguments**

Outer calls resolve; hints inside the expansion are still missing

```snap
tests/snap/inlay_hint/parameter_hints/19_param_pack_constructors.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Hints

<!-- BEGIN GENERATED ITEMS: type_hints -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced `auto` variables**

The hint shows the full variable type, qualifiers included

```snap
tests/snap/inlay_hint/type_hints/01_type_auto.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1298 clangd#1357 -->

**Type sugar length limits**

Aliases keep their spelling; over-long types fall back to the sugared name

```snap
tests/snap/inlay_hint/type_hints/02_type_sugar.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

Each binding hints its canonical type; the aggregate itself stays bare

```snap
tests/snap/inlay_hint/type_hints/03_type_structured_bindings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1163 -->

**Lambdas**

Variables, deduced return types, and init-captures all hint

```snap
tests/snap/inlay_hint/type_hints/04_type_lambdas.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced return types**

Deduced return types appear as `-> T` after the parameter list

```snap
tests/snap/inlay_hint/type_hints/05_type_auto_return.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` spellings**

The underlying type shows next to the written `decltype`

```snap
tests/snap/inlay_hint/type_hints/06_type_decltype.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` parameters**

A template with exactly one instantiation reveals the deduced type

```snap
tests/snap/inlay_hint/type_hints/07_type_auto_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1749 -->

**Explicitly spelled initializers**

Casts and functional casts still hint redundantly

```snap
tests/snap/inlay_hint/type_hints/08_type_explicit_source.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2275 -->

**Dependent `auto`**

Deduction inside an uninstantiated template body stays silent

```snap
tests/snap/inlay_hint/type_hints/09_type_dependent.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Scope suppression**

Namespace qualifiers drop from hints; class scopes stay

```snap
tests/snap/inlay_hint/type_hints/10_type_scopes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Tuple-protocol bindings**

Hints print the canonical type, not `tuple_element<I, T>::type`

```snap
tests/snap/inlay_hint/type_hints/11_type_bindings_tuple.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2275 -->

**Instantiated templates**

Instantiated bodies repeat no hints at the pattern; dependent `auto` could
reveal the deduced type while exactly one instantiation exists

```snap
tests/snap/inlay_hint/type_hints/12_type_conflicting_instantiations.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Designator Hints

<!-- BEGIN GENERATED ITEMS: designator_hints -->

<!-- BEGIN CAPABILITY: supported clangd#2303 -->

**Field and index designators**

Positional aggregate initialization shows `.field=` and `[index]=`

```snap
tests/snap/inlay_hint/designator_hints/01_designator_basic.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested aggregates**

Written braces recurse; omitted braces flatten into `.outer.inner=`

```snap
tests/snap/inlay_hint/designator_hints/02_designator_nested.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous members**

Unnamed unions and structs vanish from the designator path

```snap
tests/snap/inlay_hint/designator_hints/03_designator_anonymous.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Designator suppression**

Written designators and `/*name=*/` comments keep their inits bare

```snap
tests/snap/inlay_hint/designator_hints/04_designator_suppression.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Aggregates only**

Constructor calls, copies and idiomatic zero-init produce no designators

```snap
tests/snap/inlay_hint/designator_hints/05_designator_aggregates_only.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Broken initializers**

Designators survive next to initializers that fail to compile

```snap
tests/snap/inlay_hint/designator_hints/06_designator_recovery.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2540 -->

**Parenthesized aggregate initialization**

C++20 `Point(1, 2)` gets no hints yet

```snap
tests/snap/inlay_hint/designator_hints/07_designator_parenthesized.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Hint Kinds

<!-- BEGIN GENERATED ITEMS: other_hint_kinds -->

<!-- BEGIN CAPABILITY: unsupported clangd#2583 -->

**Template parameter hints**

Template argument hints are not emitted at call sites yet

```snap
tests/snap/inlay_hint/other_hint_kinds/01_template_parameter_hints.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2331 -->

**CTAD arguments**

CTAD does not display deduced class template arguments yet

```snap
tests/snap/inlay_hint/other_hint_kinds/02_ctad_arguments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2254 -->

**Implicit conversion hints**

Implicit conversions at call sites have no hints yet

```snap
tests/snap/inlay_hint/other_hint_kinds/03_conversion_hints.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Block End Hints

Off by default (`inlay_hints.block_end`). After the closing brace of a block spanning at least two lines, clice shows the name of what the brace closes — functions, types, namespaces, and control-flow statements:

```cpp
void Widget::process(const Config& cfg) {
    // ...
} // Widget::process

namespace detail {
    // ...
} // namespace detail

while (running) {
    // ...
} // while running
```

Condition summaries print for `if`/`while`/`switch`/`for` where a short spelling exists; an `else if` chain hints as plain `// if`. Labels longer than 60 characters are dropped.

A related idea, `#endif` hints showing the matching condition ([clangd#2487](https://github.com/clangd/clangd/issues/2487)), is not implemented.

## Default Argument Hints

Off by default (`inlay_hints.default_arguments`). Call sites that rely on default arguments show what was omitted, abbreviated past the type-name limit:

```cpp
void log(int level, bool flush = true, int repeat = 1);
log(2);
//     ^ , flush: true, repeat: 1
```

## Configuration

The `[inlay_hints]` section of `clice.toml` (or the same keys via `initializationOptions`) controls every category: `enabled`, `parameters`, `deduced_types`, `designators`, `block_end`, `default_arguments`, and `type_name_limit`. See the [configuration guide](../guide/configuration.md#inlay-hints) for details. Configuration changes take effect after a server restart — a recompile is never involved.

## Interactive Behavior

- Requests are range-scoped: hints outside the requested range are discarded.
- Parameter hints anchor to the left of their argument; type and designator hints anchor to their declaration side with LSP padding flags instead of embedded spaces.
- Identical duplicate hints (e.g. from template instantiations) collapse into one.

## Other Known Gaps

- [ ] Abbreviated type hints with expandable label parts via `InlayHintLabelPart` ([clangd#2269](https://github.com/clangd/clangd/issues/2269))
- [ ] Clickable type names — go-to-definition on the hinted type ([clangd#1535](https://github.com/clangd/clangd/issues/1535))
- [ ] Scope-aware type shortening — print `Bar` instead of `foo::Bar` inside `namespace foo` ([clangd#2270](https://github.com/clangd/clangd/issues/2270))
- [ ] Parameter hints lost when a coroutine returns a template type ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
