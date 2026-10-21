# Semantic Tokens

<!-- The cards in the sections below are generated from the documented cases
     in tests/semantic_tokens/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice colors every token of a document: names by what they declare or name,
keywords, literals, comments, operators and directives by their spelling. It
speaks a vocabulary of its own, made for C and C++, rather than the standard
LSP one: a type alias, a template type parameter and a non-type template
parameter are three kinds, and a modifier such as `controlFlow` says what a
keyword, a function or a macro does to the flow of control. The clice
extensions for VS Code and Neovim color all of it; any other editor colors
the names it knows, and the configuration renames clice's to those.

## Kinds

| Kind                               | Marks                                                                                                         |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `comment`                          | a comment                                                                                                     |
| `string`, `character`, `number`    | a literal, its prefix and suffix included                                                                     |
| `escape`                           | an escape sequence in a literal: `\n`, `\x41`, `{{` in a format string                                        |
| `formatSpecifier`                  | a placeholder of a format string: `{}`, `{:>8}`, `%d`                                                         |
| `keyword`                          | a keyword; `module`, `import`, `override` and `final` where they act as one                                   |
| `primitive`                        | a built-in type: `int`, `bool`, `char8_t`                                                                     |
| `operator`                         | an operator, built-in or overloaded, words such as `sizeof`, `new` and `and` included                         |
| `bracket`                          | the `<` and `>` around template arguments and parameters                                                      |
| `directive`, `header`              | a preprocessor directive; a header name                                                                       |
| `namespace`, `module`              | a namespace or namespace alias; a C++20 module name                                                           |
| `class`, `struct`, `union`, `enum` | a class by the keyword that declares it                                                                       |
| `typeAlias`                        | a `typedef` or `using` alias                                                                                  |
| `typeParameter`                    | a template type or template template parameter                                                                |
| `templateParameter`                | a non-type template parameter                                                                                 |
| `concept`                          | a concept                                                                                                     |
| `function`, `method`               | a free function; a member function, constructors and destructors included                                     |
| `variable`, `parameter`, `field`   | a variable, a function parameter, a data member                                                               |
| `enumMember`, `label`              | an enumerator; a label                                                                                        |
| `macro`, `macroParameter`          | a macro; a parameter of a function-like macro, in its definition                                              |
| `attribute`                        | an attribute name: `nodiscard`, `gnu::packed`                                                                 |
| `unknown`                          | a dependent name only an instantiation resolves: `T::reset()`, `value.size()`                                 |
| `conflict`                         | a name meaning entities of different kinds at once                                                            |
| `plain`                            | a token in an untaken branch with no kind of its own, or any with lexical tokens off, sent for its `inactive` |

## Modifiers

A modifier named like a standard LSP one means what the standard one means,
on every kind:

| Modifier         | Means                                                                                                                       |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `declaration`    | declares the entity without defining it                                                                                     |
| `definition`     | defines the entity                                                                                                          |
| `readonly`       | the object cannot change through the name: a constant, a reference to const, a const pointer, an enumerator, a const method |
| `static`         | a static member or a static local                                                                                           |
| `deprecated`     | the entity is deprecated                                                                                                    |
| `abstract`       | a pure virtual method, or a class with one                                                                                  |
| `defaultLibrary` | declared in a system header or built into the compiler                                                                      |
| `documentation`  | inside a documentation comment                                                                                              |
| `modification`   | the entity changes there: assigned, incremented, passed by reference or address to non-const                                |

A name a declaration gives carries where it is visible from: `local` (in its
function or block), `member` (in its class), `internal` (in its translation
unit, as `static` and unnamed namespaces make it) or `global`.

The rest are clice's own, and some mean something of their own on each kind:

| Modifier                    | On                                          | Means                                                                                 |
| --------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------------- |
| `template`                  | a class, alias, variable or function        | a template, or a specialization of one                                                |
| `dependent`                 | any name                                    | depends on a template parameter; every `unknown` name carries it                      |
| `virtual`                   | a method; a class                           | a virtual method; a polymorphic class                                                 |
| `constexpr`                 | a function, variable or `templateParameter` | `constexpr`, `consteval` or `constinit`, or a non-type template parameter             |
| `constructor`, `destructor` | a method                                    | a constructor; a destructor                                                           |
| `callable`                  | a macro                                     | a function-like macro                                                                 |
|                             | a variable, parameter or field              | of a type that can be called: a function pointer, a lambda, a class with `operator()` |
| `controlFlow`               | a keyword                                   | transfers control: `if`, `for`, `return`, `throw`, `co_await`, `case`                 |
|                             | a function                                  | `[[noreturn]]`                                                                        |
|                             | a macro                                     | its expansion can leave the function: `return`, `break`, `goto`, `throw`              |
| `userDefined`               | an operator; a literal's suffix             | calls an overloaded operator; a user-defined literal                                  |
| `declarator`                | an operator                                 | the `*`, `&` or `&&` of a declarator, apart from the same operators in expressions    |
| `literal`                   | a keyword                                   | `true`, `false`, `nullptr`                                                            |
| `affix`                     | a literal                                   | its prefix or suffix: `u8`, `L`, `42u`, `1.5f`                                        |
| `empty`                     | a macro                                     | expands to nothing                                                                    |
| `inactive`                  | any token                                   | in an untaken preprocessor branch, or an argument a macro drops                       |

## Editors

- **VS Code**: the clice extension declares every kind and modifier with the
  TextMate scopes themes color, so `keyword.controlFlow` takes a theme's color
  for control keywords and `escape` its color for escape sequences.
- **Neovim**: the clice plugin links the `@lsp.type.*` and `@lsp.mod.*`
  groups of clice's names to the groups colorschemes style, as defaults a
  colorscheme can override.
- **Other editors** — Zed, Kate, Emacs, Sublime Text — color the standard LSP
  names. Renaming clice's kinds to the standard names closest to them colors
  them there; the renames go in that editor's `initializationOptions` for
  clice, not in a `clice.toml` VS Code reads too:

```json
{
  "semantic_tokens": {
    "kinds": {
      "character": "string",
      "escape": "string",
      "formatSpecifier": "string",
      "primitive": "type",
      "bracket": "operator",
      "directive": "macro",
      "header": "string",
      "module": "namespace",
      "union": "struct",
      "typeAlias": "type",
      "templateParameter": "parameter",
      "concept": "type",
      "field": "property",
      "macroParameter": "parameter",
      "attribute": "decorator"
    }
  }
}
```

Zed asks for semantic tokens only once `semantic_tokens` is `"combined"` (or
`"full"`) for C and C++; the mapping goes under
`lsp.clice.initialization_options`.

## Configuration

The `[semantic_tokens]` section of `clice.toml`, or the same keys in the
editor's `initializationOptions`, holds the switch for lexical tokens and the
two renaming tables. The names are announced once, when the editor connects:
they come from the editor's settings and the first workspace folder's
`clice.toml`, and a change takes a server restart. See the
[configuration guide](../guide/configuration.md#semantic-tokens) for details.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/options -->

<!-- BEGIN CAPABILITY: supported -->

**Lexical tokens**

With `lexical` off, only what the editor's own grammar cannot tell is sent

Keywords, literals, comments, built-in operators and directives are left
to the editor's grammar. Names, template brackets and declarator operators
are still sent, and so is the code of untaken branches, as `plain` tokens:
the grammar's color stays and only dims.

```case
tests/semantic_tokens/options/01_lexical_tokens
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Kind names**

`kinds` renames the token kinds the server announces

Two kinds given one name merge into it, and a kind renamed to `""` is not
sent, leaving the editor's own color. The names are announced once, when the
editor connects: a change takes a restart.

```case
tests/semantic_tokens/options/02_kind_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Modifier names**

`modifiers` renames the token modifiers the server announces

A modifier renamed to `""` is never set. Like the kinds, the names are
announced once, when the editor connects.

```case
tests/semantic_tokens/options/03_modifier_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Lexical Tokens

Kinds derived from the token stream itself, independent of the AST.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/lexical -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

Line, block and documentation comments receive comment tokens, a documentation comment with the `documentation` modifier

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

Alternative operator spellings are operators, and contextual specifiers keep keyword tokens

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

<!-- BEGIN CAPABILITY: supported -->

**Literal prefixes and suffixes**

A literal's prefix and suffix are tokens of their own, with the `affix` modifier

They keep the kind of their literal; a user-defined literal's suffix also
carries `userDefined`.

```case
tests/semantic_tokens/lexical/05_literal_affixes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Escape sequences**

Escape sequences inside literals are `escape` tokens

```case
tests/semantic_tokens/lexical/06_escape_sequences
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1421 -->

**Declarator vs operator disambiguation**

A declarator's `*`, `&` and `&&` carry the `declarator` modifier, apart from the same operators in expressions

```case
tests/semantic_tokens/lexical/07_declarator_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Bracket token types**

The angle brackets of template arguments and parameters are `bracket` tokens

They are told apart from the comparisons an editor's grammar takes them
for. Parentheses, square brackets and braces, which the editor matches
itself, get no token.

```case
tests/semantic_tokens/lexical/08_brackets
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2209 -->

**Attribute names**

Attribute names are `attribute` tokens, and their arguments keep their own kinds

```case
tests/semantic_tokens/lexical/09_attributes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Format strings**

The placeholders of a format string are `formatSpecifier` tokens

A string passed where a function takes a format string — a
`basic_format_string` parameter as `std::format`, `std::print` and {fmt}
declare it, or the argument a `format(printf, …)` or `format(scanf, …)`
attribute names — splits into its text and its placeholders: `{}` and
`{:>8.2f}` in the first style, `%d` and `%-8.3f` in printf's, `%ms` and
`%15[^,\n]` in scanf's, a raw string's too. Escaped braces and `%%` are
escapes; a string no function reads as a format is plain text.

```case
tests/semantic_tokens/lexical/12_format_strings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Documentation comments**

Commands in documentation comments are keywords, the names they document parameters

Inside a `///` or `/** */` comment, a command such as `\param`, `@brief`
or `@return` is a `keyword` and the name after `@param` a `parameter`,
both with the `documentation` modifier; the rest of the comment stays a
`comment`. An ordinary comment has no commands.

```case
tests/semantic_tokens/lexical/13_doc_comments
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

A type parameter is a `typeParameter`, a non-type one a `templateParameter`, and a template carries `template`

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

<!-- BEGIN CAPABILITY: supported clangd#154 clangd#297 -->

**Dependent names**

Dependent names resolve through known primary templates

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration: they are `unknown`, or a type
after `typename`. Every dependent name carries `dependent`.

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

<!-- BEGIN CAPABILITY: supported clangd#1390 clangd#1627 clangd#2235 clangd#2547 clangd#484 -->

**Spelling forms**

Names keep their kinds in the less common ways C++ spells them

An `auto` parameter is a keyword, not a template parameter; `new` behind
`::` an operator; the class of a pointer to member a class; the left
operand of an overloaded `=` is modified like a built-in one's; a member
template named after `template` is `unknown` until an instantiation.

```case
tests/semantic_tokens/references/14_spelling_forms
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Special Members

Constructors, destructors and operator functions read as the methods they are, with modifiers of their own.

<!-- BEGIN GENERATED ITEMS: semantic_tokens/special_members -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**Constructors and destructors**

Constructors and destructors use method tokens with dedicated modifiers

A destructor's whole name, the `~` and the class name, is one method
token.

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

Readonly is about the object a name stands for: a reference to const and a
const pointer are readonly, a pointer to const is not.

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

<!-- BEGIN CAPABILITY: supported clangd#352 -->

**Scope modifiers**

A declared name carries where it is visible from: its function, its class, its file or everywhere

```case
tests/semantic_tokens/modifiers/07_scopes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#839 -->

**Mutable arguments**

An argument the callee may modify, through a reference or a pointer to non-const, carries `modification`, as the target of an assignment does

```case
tests/semantic_tokens/modifiers/08_mutable_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1521 -->

**User-defined operators**

A call to an overloaded operator is an operator token with the `userDefined` modifier

```case
tests/semantic_tokens/modifiers/10_overloaded_operators
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Callable variables**

A variable, parameter or field whose type can be called is `callable`

Function pointers, pointers to member functions, lambdas and objects of a
class with an `operator()`, its own or a base's, all carry it, at their
declaration and at every use.

```case
tests/semantic_tokens/modifiers/12_callable
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Templates**

A class, alias, variable or function template is `template` wherever it is named

So is a specialization of it, and the `<` and `>` around template arguments
are `bracket` tokens, told apart from comparisons.

```case
tests/semantic_tokens/modifiers/13_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Control flow**

Keywords that transfer control and functions that never return are `controlFlow`

`if`, `for`, `return` and the other statements that branch, loop or leave
carry it, telling them apart from declaration keywords such as `int` or
`const`. A `[[noreturn]]` function carries it too, as does a macro whose
expansion can leave the function.

```case
tests/semantic_tokens/modifiers/14_control_flow
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

Text with no kind of its own becomes a `plain` token, so even a lone `}`
line dims.

```case
tests/semantic_tokens/preprocessor/03_inactive_branches
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expansion sites and arguments**

Expansion names are macros, written arguments keep their semantics, definition bodies stay lexical but for their parameters

```case
tests/semantic_tokens/preprocessor/04_macro_expansions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2649 -->

**Object-like vs function-like macros**

A function-like macro carries `callable`, an object-like one does not

```case
tests/semantic_tokens/preprocessor/05_macro_kinds
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Empty and control-flow macros**

A macro that expands to nothing is `empty`, one that can leave the function `controlFlow`

Both modifiers mark the macro's name at its definition and at every
expansion. The arguments an empty macro drops never reach the compiler
and are `inactive`, like the code of an untaken branch.

```case
tests/semantic_tokens/preprocessor/08_empty_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro names in conditions**

A name a condition tests is a `macro`, defined or not

`#ifdef`, `#ifndef`, `defined(…)`, `#undef` and the names an `#if` tests
name macros whether or not one is defined; `defined` and checks such as
`__has_include` are operators, what such a check tests is no macro, and the
file `__has_include` tests is a header name.

```case
tests/semantic_tokens/preprocessor/09_condition_operands
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Arguments a macro drops**

The arguments of parameters a macro's definition never names are `inactive`

Like the code of an untaken branch, they never reach the compiler: all of
them for a macro that expands to nothing, the second one here for `FIRST`.

```case
tests/semantic_tokens/preprocessor/11_dropped_arguments
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
without a kind of their own in dead code — bare identifiers and plain
punctuation — are sent as the unstyled `plain` kind, giving the whole region
token coverage. The arguments a macro drops, such as those of an `assert`
compiled away, dim the same way. The clice VS Code extension renders the
regions dimmed out of the box; other editors style the modifier directly
(e.g. `@lsp.mod.inactive` in Neovim).

- [x] Dim inactive preprocessor branches ([clangd#132](https://github.com/clangd/clangd/issues/132))
- [x] Correct inactive boundaries with `#elif` chains ([clangd#602](https://github.com/clangd/clangd/issues/602))
- [x] Preserve syntax highlighting within inactive regions ([clangd#1664](https://github.com/clangd/clangd/issues/1664))
- [x] Keep inactive regions distinct from comments ([clangd#1545](https://github.com/clangd/clangd/issues/1545))

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

## Protocol Support

- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [x] Range requests (`textDocument/semanticTokens/range`): the tokens a range overlaps
- [x] Delta updates (`textDocument/semanticTokens/full/delta`): the changes since the answer the editor holds
- [x] UTF-16 positions, or UTF-8 for an editor that offers them; a token spanning lines split per line

## Known Limitations

- While a file's compile is in progress — when it is first opened, or after
  a header it includes changed — its tokens come from the project index. They
  name everything the finished compile names, but leave out what only the
  compile knows: dependent and `unknown` names, declarator operators, the
  brackets of a lambda's template parameters, format string placeholders,
  untaken branches and the arguments macros drop. These appear once the
  compile finishes.
- Code no control flow reaches, and the branch of an `if constexpr` an
  instantiation discards, are not dimmed: the first takes a control flow
  analysis, the second differs from one instantiation to the next
  ([clangd#1828](https://github.com/clangd/clangd/issues/1828)).
