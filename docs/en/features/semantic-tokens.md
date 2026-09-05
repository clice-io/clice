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

line, block and doc comments, including multiline blocks

```snap
tests/snap/semantic_tokens/lexical_tokens/01_comments.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Literals**

numbers, characters and strings, including raw strings

```snap
tests/snap/semantic_tokens/lexical_tokens/02_literals.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keywords**

Including alternative operator spellings and the contextual `final` / `override`

```snap
tests/snap/semantic_tokens/lexical_tokens/03_keywords.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Preprocessor directives**

`#if` chains keep directive kinds; disabled branches keep lexical kinds; pragma arguments stay plain

```snap
tests/snap/semantic_tokens/lexical_tokens/04_directives.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive regions**

Tokens in untaken branches keep their lexical kinds and carry the `inactive` modifier; unclassified tokens become plain `identifier` carriers, so even a lone `}` line dims

```snap
tests/snap/semantic_tokens/lexical_tokens/05_inactive_regions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Header names**

Quoted and angled `#include` filenames, including the split `# include` form

```snap
tests/snap/semantic_tokens/lexical_tokens/06_include_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive regions at the top of a file**

Untaken branches among the leading directives dim the same way

```snap
tests/snap/semantic_tokens/lexical_tokens/07_inactive_preamble.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Literal prefixes and suffixes**

Encoding prefixes, type suffixes, digit separators and UDL suffixes as distinct tokens

```snap
tests/snap/semantic_tokens/lexical_tokens/08_literal_affixes.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Escape sequences**

Highlighted distinctly inside string and character literals

```snap
tests/snap/semantic_tokens/lexical_tokens/09_escape_sequences.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1421 -->

**Declarator vs operator disambiguation**

`*`, `&`, `&&` as declarators vs arithmetic/logical operators

```snap
tests/snap/semantic_tokens/lexical_tokens/10_declarator_operators.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Primitive token type**

A distinct kind for built-in types instead of plain `keyword`

```snap
tests/snap/semantic_tokens/lexical_tokens/11_primitive_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Bracket token types**

Matching `()`, `[]`, `{}`, `<>` pairs as distinct kinds

```snap
tests/snap/semantic_tokens/lexical_tokens/12_bracket_pairs.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Declarations & References

Names classified by the declaration they define or reference.

<!-- BEGIN GENERATED ITEMS: declarations_references -->

<!-- BEGIN CAPABILITY: supported -->

**Namespaces**

definitions, references, nested namespaces and namespace aliases

```snap
tests/snap/semantic_tokens/declarations_references/01_namespaces.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Types**

class, struct, union, enum and type aliases, at definitions and references

```snap
tests/snap/semantic_tokens/declarations_references/02_types.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functions and methods**

declarations, definitions and call sites

```snap
tests/snap/semantic_tokens/declarations_references/03_functions.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variables**

globals, locals, parameters, fields and enum members

```snap
tests/snap/semantic_tokens/declarations_references/04_variables.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Templates**

Type and non-type template parameters, with the `templated` modifier on template names

```snap
tests/snap/semantic_tokens/declarations_references/05_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concepts**

Definitions and uses as template constraints

```snap
tests/snap/semantic_tokens/declarations_references/06_concepts.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Labels**

`goto` targets and label definitions

```snap
tests/snap/semantic_tokens/declarations_references/07_labels.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

Binding names at definition and use

The opening `[` deliberately carries no token; only the binding names
themselves are highlighted.

```snap
tests/snap/semantic_tokens/declarations_references/08_structured_bindings.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#122 -->

**Member initializer lists**

Initialized fields highlighted as fields

```snap
tests/snap/semantic_tokens/declarations_references/09_member_init_list.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2619 -->

**Using declarations**

The introduced name keeps its target's kind

```snap
tests/snap/semantic_tokens/declarations_references/10_using_declarations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#868 -->

**Lambda init-captures**

The captured name highlighted as a variable

```snap
tests/snap/semantic_tokens/declarations_references/11_lambda_init_capture.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#213 -->

**`sizeof...`**

The pack parameter keeps its type-parameter token

```snap
tests/snap/semantic_tokens/declarations_references/12_sizeof_pack.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1283 -->

**`using enum`**

The enum name highlighted at the using site

```snap
tests/snap/semantic_tokens/declarations_references/13_using_enum.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduction guides**

The guide name and the guided template highlighted

```snap
tests/snap/semantic_tokens/declarations_references/14_deduction_guides.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#316 -->

**Explicit instantiation**

The instantiated template name and its written template arguments highlighted, on the extern declaration and the definition alike

```snap
tests/snap/semantic_tokens/declarations_references/15_explicit_instantiation_class.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#154 clangd#297 -->

**Dependent names**

Resolved through the primary template where one is known

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration and currently get no token;
heuristic coloring for such names remains open.

```snap
tests/snap/semantic_tokens/declarations_references/16_dependent_names.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable templates**

declarations, definitions, partial and full specializations

```snap
tests/snap/semantic_tokens/declarations_references/17_variable_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Out-of-line member definitions**

Qualified names keep method kinds and modifiers

```snap
tests/snap/semantic_tokens/declarations_references/18_out_of_line_methods.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Alias templates**

The alias name carries the type kind and the `templated` modifier

```snap
tests/snap/semantic_tokens/declarations_references/19_alias_templates.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template template parameters**

Declared and used as types

```snap
tests/snap/semantic_tokens/declarations_references/20_template_template_params.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda captures**

by-copy and by-reference captures reference the captured variable; `this` stays a keyword

```snap
tests/snap/semantic_tokens/declarations_references/21_lambda_captures.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Range-based for**

The loop variable at definition and use

```snap
tests/snap/semantic_tokens/declarations_references/22_range_for.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum underlying types**

The enum-base reference keeps its type kind

```snap
tests/snap/semantic_tokens/declarations_references/23_enum_base.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend declarations**

Befriended names resolve to their targets; inline friends define

```snap
tests/snap/semantic_tokens/declarations_references/24_friend_declarations.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Dependent using declarations**

`using T::name` in a template body

The introduced name and its uses currently get no token; the reserved
dependent-name modifier is not emitted yet.

```snap
tests/snap/semantic_tokens/declarations_references/25_dependent_using.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Function explicit instantiation directives**

Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments and the parameter types

```snap
tests/snap/semantic_tokens/declarations_references/26_explicit_instantiation_function.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Variable explicit instantiation directives**

Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments, even the declarator's type

```snap
tests/snap/semantic_tokens/declarations_references/27_explicit_instantiation_variable.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Explicit instantiation member bodies**

A dependent name paints as its actual resolution: agreeing kinds keep the modifiers all instantiations share, disagreeing kinds paint a conflict

```snap
tests/snap/semantic_tokens/declarations_references/28_explicit_instantiation_member_bodies.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Modules

<!-- BEGIN GENERATED ITEMS: modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module declarations**

The contextual `module` keyword, dotted module names and the private fragment

```snap
tests/snap/semantic_tokens/modules/01_modules.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module partitions**

Partition names in the module declaration

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

The modifier distinguishes the two

```snap
tests/snap/semantic_tokens/token_modifiers/01_decl_def_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static**

class-level members and static locals

```snap
tests/snap/semantic_tokens/token_modifiers/02_static_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Readonly**

Const and constexpr values, const methods and enum members

Readonly is currently value-based: a pointer to const counts as
readonly even though the pointer itself can change.

```snap
tests/snap/semantic_tokens/token_modifiers/03_readonly_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Virtual and abstract**

Virtual methods, pure virtual methods and abstract classes

```snap
tests/snap/semantic_tokens/token_modifiers/04_virtual_abstract.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated**

`[[deprecated]]` declarations and their uses

```snap
tests/snap/semantic_tokens/token_modifiers/05_deprecated_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default library**

Symbols declared in system headers

```snap
tests/snap/semantic_tokens/token_modifiers/06_default_library.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#352 -->

**Scope modifiers**

function, class, file and global scope

```snap
tests/snap/semantic_tokens/token_modifiers/07_scope_modifiers.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#839 -->

**Mutable reference and pointer**

Arguments passed by non-const reference or pointer

```snap
tests/snap/semantic_tokens/token_modifiers/08_mutable_reference.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Deduced**

Mark deduced types such as `auto` and `decltype`

```snap
tests/snap/semantic_tokens/token_modifiers/09_deduced_modifier.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1521 -->

**User-defined operators**

Distinguish overloaded operators from built-in ones

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

The class name used as a constructor call inside the class

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

Method tokens with the constructor/destructor modifier

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

special-member names keep their definition tokens

```snap
tests/snap/semantic_tokens/token_correctness/07_defaulted_deleted.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Attributes

<!-- BEGIN GENERATED ITEMS: attributes -->

<!-- BEGIN CAPABILITY: unsupported clangd#2209 -->

**Attribute names**

Standard and vendor attributes, and expressions inside them

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

```snap
tests/snap/semantic_tokens/macros/01_macro.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expansion sites and arguments**

Expansion names are macros, written arguments keep their semantics, definition bodies stay lexical

```snap
tests/snap/semantic_tokens/macros/02_macro_expansion.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2649 -->

**Object-like vs function-like macros**

Distinct highlighting for the two forms

```snap
tests/snap/semantic_tokens/macros/03_macro_kinds.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Known Gaps

Curated issues without a fixture yet:

- `auto` parameters must not be highlighted as template type parameters
  ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- Nested name specifier in a pointer-to-member should get a token
  ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- `::new` should keep the `new` keyword highlighted
  ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- `co_yield` / `co_await` lose highlighting when the coroutine return type is
  a template ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- Token modifiers should apply to operands of overloaded operators
  ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- Dependent template names (`obj.template get<int>()`), members imported from
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
