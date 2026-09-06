# Semantic Tokens

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice classifies every token of a document with its own token-kind vocabulary,
which is richer than the standard LSP token types and consistent across all
clice replies. Clients that prefer standard LSP kinds can map them through
configuration.

## Lexical Tokens

Kinds derived from the token stream itself, independent of the AST.

<!-- BEGIN GENERATED ITEMS: lexical_tokens -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

Line, block and documentation comments receive comment tokens

```snap
tests/snap/semantic_tokens/lexical_tokens/01_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Literals**

Numbers, characters and strings receive literal tokens

```snap
tests/snap/semantic_tokens/lexical_tokens/02_literals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keywords**

Alternative operator spellings and contextual specifiers retain keyword
tokens

```snap
tests/snap/semantic_tokens/lexical_tokens/03_keywords.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Preprocessor directives**

`#if` chains keep directive kinds; disabled branches keep lexical kinds;
pragma arguments stay plain

```snap
tests/snap/semantic_tokens/lexical_tokens/04_directives.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive regions**

Tokens in untaken branches keep their lexical kinds and carry the `inactive`
modifier; unclassified tokens become plain `identifier` carriers, so even a
lone `}` line dims

```snap
tests/snap/semantic_tokens/lexical_tokens/05_inactive_regions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Header names**

Quoted and angled include filenames receive string tokens

```snap
tests/snap/semantic_tokens/lexical_tokens/06_include_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive preamble regions**

Untaken branches among the leading directives dim the same way

```snap
tests/snap/semantic_tokens/lexical_tokens/07_inactive_preamble.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Literal prefixes and suffixes**

Literal prefixes, suffixes and separators do not receive distinct tokens
yet

```snap
tests/snap/semantic_tokens/lexical_tokens/08_literal_affixes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Escape sequences**

Escape sequences are not highlighted distinctly inside literals yet

```snap
tests/snap/semantic_tokens/lexical_tokens/09_escape_sequences.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1421 -->

**Declarator vs operator disambiguation**

Declarator and expression operators do not receive distinct token kinds yet

```snap
tests/snap/semantic_tokens/lexical_tokens/10_declarator_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Primitive token type**

Built-in types use a distinct token kind instead of plain `keyword`

```snap
tests/snap/semantic_tokens/lexical_tokens/11_primitive_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Bracket token types**

Matching brackets do not receive pair-specific token kinds yet

```snap
tests/snap/semantic_tokens/lexical_tokens/12_bracket_pairs.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Declarations

Names classified by the declaration they define or reference.

<!-- BEGIN GENERATED ITEMS: declarations -->

<!-- BEGIN CAPABILITY: supported -->

**Namespaces**

Namespace definitions, references, nesting and aliases receive namespace
tokens

```snap
tests/snap/semantic_tokens/declarations/01_namespaces.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Types**

Type definitions and references keep their respective type kinds

```snap
tests/snap/semantic_tokens/declarations/02_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functions and methods**

Function declarations, definitions and calls receive function tokens

```snap
tests/snap/semantic_tokens/declarations/03_functions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variables**

Variable declarations and references keep their respective variable kinds

```snap
tests/snap/semantic_tokens/declarations/04_variables.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Templates**

Template parameters receive type or variable kinds, and template names carry
`templated`

```snap
tests/snap/semantic_tokens/declarations/05_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concepts**

Concept definitions and constraint uses receive concept tokens

```snap
tests/snap/semantic_tokens/declarations/06_concepts.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Labels**

Labels and their `goto` references receive label tokens

```snap
tests/snap/semantic_tokens/declarations/07_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

Structured binding names receive variable tokens at definition and use

The opening `[` deliberately carries no token; only the binding names
themselves are highlighted.

```snap
tests/snap/semantic_tokens/declarations/08_structured_bindings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#868 -->

**Lambda init-captures**

Lambda init-captures receive variable tokens

```snap
tests/snap/semantic_tokens/declarations/09_lambda_init_capture.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduction guides**

Deduction guides and their guided templates receive type tokens

```snap
tests/snap/semantic_tokens/declarations/10_deduction_guides.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#316 -->

**Explicit instantiation**

Explicit class instantiations highlight template names and written arguments

```snap
tests/snap/semantic_tokens/declarations/11_explicit_instantiation_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable templates**

Variable template declarations, definitions and specializations receive
variable tokens

```snap
tests/snap/semantic_tokens/declarations/12_variable_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Out-of-line member definitions**

Qualified names keep method kinds and modifiers

```snap
tests/snap/semantic_tokens/declarations/13_out_of_line_methods.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Alias templates**

The alias name carries the type kind and the `templated` modifier

```snap
tests/snap/semantic_tokens/declarations/14_alias_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template template parameters**

Template-template parameters receive type tokens at declaration and use

```snap
tests/snap/semantic_tokens/declarations/15_template_template_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend declarations**

Befriended names resolve to their targets; inline friends define

```snap
tests/snap/semantic_tokens/declarations/16_friend_declarations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Function explicit instantiation directives**

Identifiers in a function explicit-instantiation directive remain unpainted

```snap
tests/snap/semantic_tokens/declarations/17_explicit_instantiation_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Variable explicit instantiation directives**

Identifiers in a variable explicit-instantiation directive remain unpainted

```snap
tests/snap/semantic_tokens/declarations/18_explicit_instantiation_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Explicit instantiation member bodies**

A dependent name paints as its actual resolution: agreeing kinds keep the
modifiers all instantiations share, disagreeing kinds paint a conflict

```snap
tests/snap/semantic_tokens/declarations/19_explicit_instantiation_member_bodies.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## References

Reference sites retain the semantic kind of the declaration they resolve to,
including names reached through language-specific lookup rules.

<!-- BEGIN GENERATED ITEMS: references -->

<!-- BEGIN CAPABILITY: supported clangd#122 -->

**Member initializer lists**

Member initializer lists highlight initialized names as fields

```snap
tests/snap/semantic_tokens/references/01_member_init_list.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2619 -->

**Using declarations**

The introduced name keeps its target's kind

```snap
tests/snap/semantic_tokens/references/02_using_declarations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#213 -->

**`sizeof...`**

The pack parameter keeps its type-parameter token

```snap
tests/snap/semantic_tokens/references/03_sizeof_pack.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1283 -->

**`using enum`**

Using declarations highlight enum names at the using site

```snap
tests/snap/semantic_tokens/references/04_using_enum.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#154 clangd#297 -->

**Dependent names**

Dependent names resolve through known primary templates

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration and currently get no token;
heuristic coloring for such names remains open.

```snap
tests/snap/semantic_tokens/references/05_dependent_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda captures**

By-copy and by-reference captures reference the captured variable; `this`
stays a keyword

```snap
tests/snap/semantic_tokens/references/06_lambda_captures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Range-based for**

Range-for variables keep variable tokens at definitions and uses

```snap
tests/snap/semantic_tokens/references/07_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum underlying types**

The enum-base reference keeps its type kind

```snap
tests/snap/semantic_tokens/references/08_enum_base.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Dependent using declarations**

Dependent using declarations remain unpainted

The introduced name and its uses currently get no token; the reserved
dependent-name modifier is not emitted yet.

```snap
tests/snap/semantic_tokens/references/09_dependent_using.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Modules

<!-- BEGIN GENERATED ITEMS: modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module declarations**

Module declarations tokenize contextual keywords, dotted names and private
fragments

```snap
tests/snap/semantic_tokens/modules/01_modules.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module partitions**

Module declarations tokenize partition names

```snap
tests/snap/semantic_tokens/modules/02_module_partition.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`module` and `import` as identifiers**

Contextual keywords keep their semantic kinds outside module declarations

```snap
tests/snap/semantic_tokens/modules/03_module_keyword_identifier.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token Modifiers

<!-- BEGIN GENERATED ITEMS: token_modifiers -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration vs definition**

Declaration and definition modifiers distinguish the two sites

```snap
tests/snap/semantic_tokens/token_modifiers/01_decl_def_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static**

Static members and locals carry the static modifier

```snap
tests/snap/semantic_tokens/token_modifiers/02_static_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Readonly**

Const values and methods, plus enum members, carry the readonly modifier

Readonly is currently value-based: a pointer to const counts as
readonly even though the pointer itself can change.

```snap
tests/snap/semantic_tokens/token_modifiers/03_readonly_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Virtual and abstract**

Virtual methods and abstract classes carry virtual or abstract modifiers

```snap
tests/snap/semantic_tokens/token_modifiers/04_virtual_abstract.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated**

Deprecated declarations and uses carry the deprecated modifier

```snap
tests/snap/semantic_tokens/token_modifiers/05_deprecated_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default library**

Symbols from system headers carry the default-library modifier

```snap
tests/snap/semantic_tokens/token_modifiers/06_default_library.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#352 -->

**Scope modifiers**

Symbols do not carry function, class, file or global scope modifiers yet

```snap
tests/snap/semantic_tokens/token_modifiers/07_scope_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#839 -->

**Mutable reference and pointer**

Mutable reference and pointer arguments do not carry a modifier yet

```snap
tests/snap/semantic_tokens/token_modifiers/08_mutable_reference.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Deduced**

Deduced types do not carry a dedicated modifier yet

```snap
tests/snap/semantic_tokens/token_modifiers/09_deduced_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1521 -->

**User-defined operators**

Overloaded operators do not differ from built-in operators yet

```snap
tests/snap/semantic_tokens/token_modifiers/10_user_defined_operator.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Conflict & Ambiguity

C++ allows structurally different entities to share one name. When a single
written name refers to entities of different kinds at once, no single token
type is correct; such names receive the dedicated **conflict** token type,
which clients typically display in a neutral color.

<!-- BEGIN GENERATED ITEMS: conflict_ambiguity -->

<!-- BEGIN CAPABILITY: supported -->

**Type vs function**

A name naming both renders as `conflict`

```snap
tests/snap/semantic_tokens/conflict_ambiguity/01_conflict_using.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type vs variable**

A name naming both renders as `conflict`

```snap
tests/snap/semantic_tokens/conflict_ambiguity/02_conflict_type_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Same-kind overload sets**

A name naming only functions is no conflict

```snap
tests/snap/semantic_tokens/conflict_ambiguity/03_using_overloads.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Injected class name**

An injected class name keeps its class token when used as a constructor

The written name renders as the class; the constructor reference it
implies paints nothing extra — the `(` stays token-free.

```snap
tests/snap/semantic_tokens/conflict_ambiguity/04_injected_class_name.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token Correctness

Shapes clice pins deliberately, including issues clangd got wrong.

<!-- BEGIN GENERATED ITEMS: token_correctness -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**Constructors and destructors**

Constructors and destructors use method tokens with dedicated modifiers

A destructor name renders as two tokens: the `~` carries the method
kind and the declaration/definition modifiers, the class name after it
stays a reference to the class.

```snap
tests/snap/semantic_tokens/token_correctness/01_constructors_destructors.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous parameters**

Unnamed parameters produce no tokens

The punctuation after an unnamed parameter's type stays token-free.

```snap
tests/snap/semantic_tokens/token_correctness/02_anonymous_parameters.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Operator names**

The `operator` keyword and call-site punctuation stay plain

An operator's written name is keyword plus punctuation, so no name
token is painted: `operator` keeps its keyword classification and
call sites emit nothing on the operator symbol.

```snap
tests/snap/semantic_tokens/token_correctness/03_operator_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Destructors of class templates**

The `~` shape holds under templates

```snap
tests/snap/semantic_tokens/token_correctness/04_template_destructor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Conversion operators**

Written as keywords, converting uses paint nothing extra

```snap
tests/snap/semantic_tokens/token_correctness/05_conversion_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pseudo-destructor on a template parameter**

The `~` paints nothing; the type name keeps its kind

```snap
tests/snap/semantic_tokens/token_correctness/06_pseudo_destructor.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Defaulted and deleted members**

Special-member names keep their definition tokens

```snap
tests/snap/semantic_tokens/token_correctness/07_defaulted_deleted.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Attributes

<!-- BEGIN GENERATED ITEMS: attributes -->

<!-- BEGIN CAPABILITY: unsupported clangd#2209 -->

**Attribute names**

Attribute names and their expressions do not receive semantic tokens yet

```snap
tests/snap/semantic_tokens/attributes/01_attributes.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

Tokens inside macro definition bodies keep their lexical kinds; highlighting
them from their expansions belongs to a future expansion-preview feature.

<!-- BEGIN GENERATED ITEMS: macros -->

<!-- BEGIN CAPABILITY: supported -->

**Macro definition and expansion**

Macro definitions and expansions receive semantic tokens

```snap
tests/snap/semantic_tokens/macros/01_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expansion sites and arguments**

Expansion names are macros, written arguments keep their semantics,
definition bodies stay lexical

```snap
tests/snap/semantic_tokens/macros/02_macro_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2649 -->

**Object-like vs function-like macros**

Object-like and function-like macros do not receive distinct token kinds yet

```snap
tests/snap/semantic_tokens/macros/03_macro_kinds.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Known Gaps

Curated issues without a fixture yet:

- [ ] `auto` parameters must not be highlighted as template type parameters
      ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- [ ] Nested name specifier in a pointer-to-member should get a token
      ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- [ ] `::new` should keep the `new` keyword highlighted
      ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- [ ] `co_yield` / `co_await` lose highlighting when the coroutine return type is
      a template ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- [ ] Token modifiers should apply to operands of overloaded operators
      ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- [ ] Dependent template names (`obj.template get<int>()`), members imported from
      a dependent base via `using`, and dependent names with mixed-kind overload
      sets ([clangd#484](https://github.com/clangd/clangd/issues/484),
      [clangd#686](https://github.com/clangd/clangd/issues/686),
      [clangd#1057](https://github.com/clangd/clangd/issues/1057))

## Inactive Code Regions

Every token inside an untaken preprocessor branch carries the `inactive`
modifier while keeping its lexical kind, so editors dim the region by
styling the modifier without losing the syntax colors underneath. Tokens
without a classification in dead code — bare identifiers and plain
punctuation — are emitted as the unstyled `identifier` type, giving the
whole region token coverage. The clice VS Code extension renders the
regions dimmed out of the box; other editors style the modifier directly
(e.g. `@lsp.mod.inactive` in Neovim).

- [x] Dim inactive preprocessor branches ([clangd#132](https://github.com/clangd/clangd/issues/132))
- [x] Correct inactive boundaries with `#elif` chains ([clangd#602](https://github.com/clangd/clangd/issues/602))
- [x] Preserve syntax highlighting within inactive regions ([clangd#1664](https://github.com/clangd/clangd/issues/1664))
- [x] Keep inactive regions distinct from comments ([clangd#1545](https://github.com/clangd/clangd/issues/1545))
- [ ] Unreachable code dimming ([clangd#1828](https://github.com/clangd/clangd/issues/1828))

## Format String Highlighting

- [ ] `std::format` / `std::print` placeholder highlighting ([clangd#1709](https://github.com/clangd/clangd/issues/1709))
- [ ] Highlight invalid format specifiers as errors

## Protocol Support

- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [x] UTF-16 delta-encoded token positions
- [ ] Range-based semantic tokens (`textDocument/semanticTokens/range`) — only
      compute tokens for the visible viewport, critical for large files
- [ ] Delta updates (`textDocument/semanticTokens/full/delta`) — send only
      changes since the previous response
