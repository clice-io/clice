# Semantic Tokens

<!-- The cards in the sections below are generated from the documented cases
     in tests/semantic_tokens/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice classifies every token of a document with its own token-kind vocabulary,
which is richer than the standard LSP token types and consistent across all
clice replies. Clients that prefer standard LSP kinds can map them through
configuration.

## Lexical Tokens

Kinds derived from the token stream itself, independent of the AST.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/lexical -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

Line, block and documentation comments receive comment tokens

```case
tests/semantic_tokens/lexical/01_comments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Literals**

Numbers, characters and strings receive literal tokens

```case
tests/semantic_tokens/lexical/02_literals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keywords**

Alternative operator spellings and contextual specifiers retain keyword tokens

```case
tests/semantic_tokens/lexical/03_keywords
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Primitive types**

Built-in types use a distinct token kind instead of plain `keyword`

```case
tests/semantic_tokens/lexical/04_primitive_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Literal prefixes and suffixes**

Literal prefixes and suffixes do not receive tokens of their own yet

```case
tests/semantic_tokens/lexical/05_literal_affixes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Escape sequences**

Escape sequences are not highlighted distinctly inside literals yet

```case
tests/semantic_tokens/lexical/06_escape_sequences
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1421 -->

**Declarator vs operator disambiguation**

Declarator and expression operators do not receive distinct token kinds yet

```case
tests/semantic_tokens/lexical/07_declarator_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Bracket token types**

Matching brackets do not receive pair-specific token kinds yet

```case
tests/semantic_tokens/lexical/08_brackets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2209 -->

**Attribute names**

Attribute names and their expressions do not receive semantic tokens yet

```case
tests/semantic_tokens/lexical/09_attributes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Declarations

Names classified by the declaration they define or reference.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/declarations -->

<!-- BEGIN CAPABILITY: supported -->

**Declared names**

Each declared name is colored by what it declares

```case
tests/semantic_tokens/declarations/01_declared_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Namespaces**

Namespace definitions, references, nesting and aliases receive namespace tokens

```case
tests/semantic_tokens/declarations/02_namespaces
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Types**

Type definitions and references keep their respective type kinds

```case
tests/semantic_tokens/declarations/03_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functions and methods**

Function declarations, definitions and calls receive function tokens

```case
tests/semantic_tokens/declarations/04_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variables**

Variable declarations and references keep their respective variable kinds

```case
tests/semantic_tokens/declarations/05_variables
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Templates**

Template parameters receive type or variable kinds, and a declared template carries `templated`

```case
tests/semantic_tokens/declarations/06_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concepts**

Concept definitions and constraint uses receive concept tokens

```case
tests/semantic_tokens/declarations/07_concepts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Labels**

Labels and their `goto` references receive label tokens

```case
tests/semantic_tokens/declarations/08_labels
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

Structured binding names receive variable tokens at definition and use

The opening `[` deliberately carries no token; only the binding names
themselves are highlighted.

```case
tests/semantic_tokens/declarations/09_structured_bindings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## References

Reference sites retain the semantic kind of the declaration they resolve to,
including names reached through language-specific lookup rules.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/references -->

<!-- BEGIN CAPABILITY: supported clangd#122 -->

**Member initializer lists**

Member initializer lists highlight initialized names as fields

```case
tests/semantic_tokens/references/01_member_initializers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2619 -->

**Using declarations**

The introduced name keeps its target's kind

```case
tests/semantic_tokens/references/02_using_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#154 clangd#297 -->

**Dependent names**

Dependent names resolve through known primary templates

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration and currently get no token;
heuristic coloring for such names remains open.

```case
tests/semantic_tokens/references/05_dependent_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda captures**

By-copy and by-reference captures reference the captured variable; `this` stays a keyword

```case
tests/semantic_tokens/references/06_lambda_captures
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Special Members

Constructors, destructors and operator functions read as the methods they are, with modifiers of their own.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/special_members -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**Constructors and destructors**

Constructors and destructors use method tokens with dedicated modifiers

A destructor name renders as two tokens: the `~` carries the method
kind and the declaration/definition modifiers, the class name after it
stays a reference to the class.

```case
tests/semantic_tokens/special_members/01_constructors_destructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Operator names**

An operator function's name keeps `operator` as a keyword, and each call to it is an operator token

```case
tests/semantic_tokens/special_members/02_operator_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token Modifiers

Modifiers add what the kind leaves out: a definition, a `static` or `const` symbol, a deprecated one, one from a system header.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/modifiers -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration vs definition**

Declaration and definition modifiers distinguish the two sites

```case
tests/semantic_tokens/modifiers/01_declaration_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static**

Static members and locals carry the static modifier

```case
tests/semantic_tokens/modifiers/02_static
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Readonly**

Const values and methods, plus enum members, carry the readonly modifier

Readonly is currently value-based: a pointer to const counts as
readonly even though the pointer itself can change.

```case
tests/semantic_tokens/modifiers/03_readonly
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Virtual and abstract**

Virtual methods and abstract classes carry virtual or abstract modifiers

```case
tests/semantic_tokens/modifiers/04_virtual_abstract
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated**

Deprecated declarations and uses carry the deprecated modifier

```case
tests/semantic_tokens/modifiers/05_deprecated
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default library**

Symbols from system headers carry the default-library modifier

```case
tests/semantic_tokens/modifiers/06_default_library
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#352 -->

**Scope modifiers**

Symbols do not carry function, class, file or global scope modifiers yet

```case
tests/semantic_tokens/modifiers/07_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#839 -->

**Mutable arguments**

An argument passed by mutable reference or pointer does not carry a modifier yet

```case
tests/semantic_tokens/modifiers/08_mutable_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Deduced types**

`auto` and `decltype` do not carry the deduced modifier yet

```case
tests/semantic_tokens/modifiers/09_deduced_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1521 -->

**User-defined operators**

A call to an overloaded operator is an operator token with the `userDefined` modifier

```case
tests/semantic_tokens/modifiers/10_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Conflict & Ambiguity

C++ allows structurally different entities to share one name. When a single
written name refers to entities of different kinds at once, no single token
type is correct; such names receive the dedicated **conflict** token type,
which clients typically display in a neutral color.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/ambiguity -->

<!-- BEGIN CAPABILITY: supported -->

**Conflicting names**

A name that means both a type and a function or variable renders as `conflict`

```case
tests/semantic_tokens/ambiguity/01_conflicts
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Preprocessor

Directives, header names and macro expansions receive tokens of their own, while the body of a macro definition keeps its lexical kinds.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/preprocessor -->

<!-- BEGIN CAPABILITY: supported -->

**Preprocessor directives**

Directives keep their directive kind, and the code of an untaken branch keeps its lexical kinds

Pragma arguments other than numbers stay plain.

```case
tests/semantic_tokens/preprocessor/01_directives
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Header names**

Quoted and angled header names receive header tokens

```case
tests/semantic_tokens/preprocessor/02_header_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Untaken branches**

Tokens in untaken branches keep their lexical kinds and carry the `inactive` modifier

Text with no kind of its own becomes a plain `identifier` token, so even a
lone `}` line dims.

```case
tests/semantic_tokens/preprocessor/03_inactive_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expansion sites and arguments**

Expansion names are macros, written arguments keep their semantics, definition bodies stay lexical

```case
tests/semantic_tokens/preprocessor/04_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2649 -->

**Object-like vs function-like macros**

Object-like and function-like macros do not receive distinct token kinds yet

```case
tests/semantic_tokens/preprocessor/05_macro_kinds
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Leading Directives

The includes and macros a file starts with are compiled apart from the rest of it, and their tokens read the same as anywhere else.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/preamble -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive preamble regions**

Untaken branches among the leading directives dim the same way

```case
tests/semantic_tokens/preamble/01_inactive_preamble
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro definition and expansion**

A macro's definition and its expansions receive macro tokens

```case
tests/semantic_tokens/preamble/02_preamble_macros
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Modules

Module declarations and imports receive tokens for their keywords and names.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module declarations**

Module declarations tokenize contextual keywords, dotted names and private fragments

```case
tests/semantic_tokens/modules/01_module_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module partitions**

Module declarations tokenize partition names

```case
tests/semantic_tokens/modules/02_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`module` and `import` as identifiers**

Contextual keywords keep their semantic kinds outside module declarations

```case
tests/semantic_tokens/modules/03_contextual_keywords
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

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

### Untaken Branches

A branch the preprocessor skips is dimmed, whichever directive opens it.

<!-- BEGIN GENERATED ITEMS: inactive_regions/branches -->

<!-- BEGIN CAPABILITY: supported -->

**Untaken branches**

Lines of a branch the preprocessor skips are dimmed, an `#else` after a taken branch included

```case
tests/inactive_regions/branches/02_else_branch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Header Contexts

In a header, the dimmed branches are those its [host](./header-contexts.md) skips.

<!-- BEGIN GENERATED ITEMS: inactive_regions/contexts -->

<!-- BEGIN CAPABILITY: supported -->

**Switching the host**

A header shows the branches its chosen host takes, and switching hosts updates the dimming at once

```case
tests/inactive_regions/contexts/01_host_switch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Edits

The dimming follows an edit to a condition as you type.

<!-- BEGIN GENERATED ITEMS: inactive_regions/updates -->

<!-- BEGIN CAPABILITY: supported -->

**Editing a condition**

Editing the macro a condition tests moves the dimming to the branch now skipped, before the file is saved

```case
tests/inactive_regions/updates/01_edited_condition
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
