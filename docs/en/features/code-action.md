# Code Action

<!-- The cards in the sections below are generated from the documented cases
     in tests/code_actions/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

clice offers code actions on a selection: refactorings that generate or reshape code from what the compiler knows about it, quick fixes for names no header declares, and the fixes the compiler and clang-tidy attach to their diagnostics. Every action is computed to completion when it is offered, so applying one never waits on a second request; edits carry the document version they were computed for, and an editor refuses them once the buffer moved on. The same actions run headless through `clice inspect code_action`.

An action anchors on the innermost construct the selection covers — a method declaration, a class name, a `switch`, an `auto` — so the list stays short: a click on a method name offers what applies to that method, a click on the class name what applies to the class.

## Running Actions

In VS Code, the actions at the cursor are behind the lightbulb and `Ctrl+.`, and the refactorings are also listed by **Refactor...** in the editor's context menu (`Ctrl+Shift+R`). Renaming a symbol, `F2`, is a request of its own rather than a code action.

Each refactoring has its own code action kind, so an editor can ask for one alone:

| Action                                               | Kind                                             |
| ---------------------------------------------------- | ------------------------------------------------ |
| Define a method inline                               | `refactor.rewrite.define.inline`                 |
| Define a function out of line, or in the host source | `refactor.rewrite.define.outOfLine`              |
| Define the missing members of a class                | `refactor.rewrite.define.missing`                |
| Implement pure virtual methods                       | `refactor.rewrite.implementPureVirtuals`         |
| Generate a memberwise constructor                    | `refactor.rewrite.generateMemberwiseConstructor` |
| Add the missing enum cases to a switch               | `refactor.rewrite.populateSwitch`                |
| Expand a deduced type                                | `refactor.rewrite.expandDeducedType`             |
| Reorder definitions by declaration order             | `refactor.rewrite.reorderDefinitions`            |
| Expand a macro                                       | `refactor.inline.macro`                          |
| Add an include, apply a diagnostic's fix             | `quickfix`                                       |

The VS Code extension has a command for each refactoring, named `clice.` followed by its kind and titled after the action in the Command Palette ("Clice: Add Missing Enum Cases to Switch"). It applies the action at once when it is the only one offered, and lets you pick otherwise. Bind a key to it in `keybindings.json`:

```json
{
  "key": "ctrl+alt+s",
  "command": "clice.refactor.rewrite.populateSwitch",
  "when": "editorLangId == cpp"
}
```

A kind also covers the kinds below it: given `refactor.rewrite.define`, VS Code's own `editor.action.codeAction` command offers every way to define the function at the cursor.

```json
{
  "key": "ctrl+alt+d",
  "command": "editor.action.codeAction",
  "args": { "kind": "refactor.rewrite.define", "apply": "ifSingle" },
  "when": "editorLangId == cpp"
}
```

In Neovim, `:LspCliceRefactor {kind}` does the same.

## Defining Functions

A declared function can be defined inline, after its class, or in the source file a header is compiled with. On a class name, every member still missing a definition is defined at once.

<!-- BEGIN GENERATED ITEMS: code_actions/define -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**Define a declared method**

A method declaration offers an inline body and an out-of-line definition after its class

The out-of-line definition repeats the declaration with `S::` before the name and drops what belongs to the declaration alone, such as default arguments.

```case
tests/code_actions/define/01_method_out_of_line
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Class template member**

The definition of a class template's member carries the template head and the template arguments in its qualifier

Default template arguments are not repeated on the head.

```case
tests/code_actions/define/04_class_template_member
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Constructors, destructors and operators**

Special member functions are defined under the class name they are spelled with

Conversion functions and operators keep their full spelling.

```case
tests/code_actions/define/06_special_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Free function declaration**

A declared free function is defined right after its declaration, in the same namespace

The name needs no qualifier inside the namespace; the return type is spelled for that scope.

```case
tests/code_actions/define/07_free_function
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Placement after existing definitions**

When the class already has out-of-line definitions in the file, the new one goes after the last of them

The qualifier follows the scope of that definition, not the class's.

```case
tests/code_actions/define/08_placement_after_definitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**Define all missing members**

On the class name, every member function without a definition is defined at once, in declaration order

Members that already have a definition, pure virtuals and defaulted members are left alone.

```case
tests/code_actions/define/09_missing_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Missing members from a definition**

Inside an out-of-line definition, the class's remaining undefined members are offered too

This is how a source file completes a class declared elsewhere: the definitions join the ones already there.

```case
tests/code_actions/define/10_missing_from_definition
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#445 -->

**Define in the host source**

In a header, a member can also be defined in the source file the header is compiled with

The definition is fully qualified and joins the class's other definitions in that file; members already defined in some source file are not offered again. Templates, inline functions and functions other files cannot see stay in the header; any other function defined there out of the class is marked `inline`.

```case
tests/code_actions/define/11_host_source
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Implementing Interfaces

A class deriving from an abstract base receives a declaration for each pure virtual method it still has to override.

<!-- BEGIN GENERATED ITEMS: code_actions/implement -->

<!-- BEGIN CAPABILITY: supported clangd#1037 -->

**Implement pure virtual methods**

A class deriving from an abstract base receives an `override` declaration for each unimplemented pure virtual method

The declarations go at the end of the class body.

```case
tests/code_actions/implement/01_pure_virtuals
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pure virtuals through a chain**

Only the methods no class in the chain implemented are declared, and a `class` gets a `public:` label for them

```case
tests/code_actions/implement/02_inheritance_chain
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Constructors

A class receives a constructor that initializes each of its fields from an argument.

<!-- BEGIN GENERATED ITEMS: code_actions/constructor -->

<!-- BEGIN CAPABILITY: supported -->

**Memberwise constructor**

A class receives a constructor taking every field in order, scalars by value and copyable classes by const reference

```case
tests/code_actions/constructor/01_memberwise
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Existing constructor not duplicated**

No constructor is generated when the class already declares one taking as many arguments as it has fields

```case
tests/code_actions/constructor/02_existing_constructor
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Single field is explicit**

A one-field class gets an `explicit` constructor, placed under a `public:` label when the class ends in another section

```case
tests/code_actions/constructor/03_single_field
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Base without a default constructor**

No constructor is generated when a base class needs its own initializer, since the memberwise one initializes fields alone

```case
tests/code_actions/constructor/05_base_without_default
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Move-only fields**

A field whose class moves but does not copy is taken by value and moved from, and an rvalue reference field binds its argument through `std::move`

The file gains `#include <utility>` when nothing declares `std::move` before the class. A class that neither copies nor moves gets no constructor.

```case
tests/code_actions/constructor/07_move_only_fields
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Switch Cases

A `switch` over an enum receives a `case` for each enumerator it does not handle yet.

<!-- BEGIN GENERATED ITEMS: code_actions/switch_cases -->

<!-- BEGIN CAPABILITY: supported clangd#807 -->

**Missing enum cases**

A switch over an enum receives the enumerators it does not handle, followed by a `break`

Enumerators sharing a handled value are considered covered.

```case
tests/code_actions/switch_cases/01_missing_cases
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Cases before the default**

With a `default` present, the missing cases go right before it and fall through into it, keeping the behavior

The action is offered from anywhere inside the switch.

```case
tests/code_actions/switch_cases/02_before_default
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unscoped enum in a namespace**

Enumerators of an unscoped enum are qualified with the enum's namespace when the switch lies outside it

```case
tests/code_actions/switch_cases/03_unscoped_enum
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Complete switch offers nothing**

A switch handling every enumerator, or one over a non-enum value, offers no action

```case
tests/code_actions/switch_cases/04_complete_switch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Deduced Types

An `auto` or a `decltype` is replaced by the type it stands for, spelled so that it names that type where it is written.

<!-- BEGIN GENERATED ITEMS: code_actions/deduced_type -->

<!-- BEGIN CAPABILITY: supported -->

**Spelled-out auto types**

An auto declaration offers to replace auto with the type it deduced

Qualifiers and declarators such as `const`, `&` and `*` stay where they are.

```case
tests/code_actions/deduced_type/01_auto_declarations
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduced return type**

A function's deduced `auto` return type expands to the deduced type, spelled for the function's scope

```case
tests/code_actions/deduced_type/02_auto_return
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expand decltype**

A `decltype` specifier expands to the type it denotes, through any `decltype` that type was itself declared with

```case
tests/code_actions/deduced_type/03_decltype
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Unnameable types stay auto**

Lambdas, dependent types, structured bindings and types the declaration cannot name are not expanded

A type cannot be named where it is local to another function, a member type the declaration has no access to, or the type of `sizeof` with no standard name for it declared yet (MSVC compatibility declares `size_t` implicitly).

```case
tests/code_actions/deduced_type/04_unnameable_types
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Names spelled for the scope**

A name drops the enclosing namespaces only as far as the shorter name still finds the same type

A name hidden by a declaration closer to the expansion keeps its qualifier, and one hidden even when fully qualified starts from the global scope.

```case
tests/code_actions/deduced_type/06_shadowed_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Standard names of builtin types**

The types of `sizeof`, a pointer difference and `nullptr` expand to their standard names where those are declared

Here a header declares them, as the standard headers do.

```case
tests/code_actions/deduced_type/07_standard_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Reordering Definitions

Out-of-line definitions are reordered to follow the order of their declarations.

<!-- BEGIN GENERATED ITEMS: code_actions/reorder -->

<!-- BEGIN CAPABILITY: supported -->

**Reorder definitions by declaration**

The out-of-line definitions of a class's members are reordered to follow the declaration order in the class

Each definition moves with the comment block directly above it.

```case
tests/code_actions/reorder/01_class_members
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Free functions by declaration order**

From a free function's definition, the definitions of the functions declared alongside it are reordered as declared

```case
tests/code_actions/reorder/03_free_functions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definitions using what lies between**

A definition stays where it is when moving it would put it before something it uses, such as a variable or macro defined between the definitions; the others are reordered around it

```case
tests/code_actions/reorder/05_dependencies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

A macro invocation is replaced by the code it expands to.

<!-- BEGIN GENERATED ITEMS: code_actions/macro -->

<!-- BEGIN CAPABILITY: supported clangd#820 -->

**Expand a macro invocation**

A macro invocation is replaced by the tokens it expands to

Arguments are substituted; the action is offered from the macro name or anywhere inside its arguments.

```case
tests/code_actions/macro/01_expand_macro
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested macros expand fully**

A macro whose body invokes other macros expands to the final tokens

```case
tests/code_actions/macro/02_nested_expansion
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Missing Includes

A name no included header declares offers the header that does, from the standard library or from the project.

<!-- BEGIN GENERATED ITEMS: code_actions/include -->

<!-- BEGIN CAPABILITY: supported clangd#1017 -->

**Standard library include**

An unresolved standard library name offers the header declaring it, from the standard library mapping

The directive goes after the includes at the top of the file.

```case
tests/code_actions/include/01_standard_library
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include for a project symbol**

A name declared in a project header the file does not include offers that header, spelled relative to the file

The candidates come from the project index: `lib.h` is known because another source file includes it.

```case
tests/code_actions/include/02_project_header
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**First include of a file**

A file without includes receives the directive at its start, after a `#pragma once` when there is one

```case
tests/code_actions/include/03_no_include_yet
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Includes under conditionals**

An include nested in a feature condition is not where a directive that must always apply goes: it follows the last one at the file's own level, or the include guard

```case
tests/code_actions/include/04_conditional_includes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Diagnostic Fixes

A diagnostic under the selection offers its fix as a quick fix, the compiler's and clang-tidy's alike. A fix that would edit inside a macro definition or another file is not offered.

<!-- BEGIN GENERATED ITEMS: code_actions/fix -->

<!-- BEGIN CAPABILITY: supported -->

**Compiler fix**

A fix the compiler attaches to its diagnostic is offered as a quick fix

The title spells out a single edit.

```case
tests/code_actions/fix/01_compiler_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy fix**

A clang-tidy finding's fix is offered as a quick fix

Without a `.clang-tidy` above the file, a small default set of checks runs.

```case
tests/code_actions/fix/02_tidy_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Fixes from notes**

Each note that carries a fix offers it as a quick fix, titled by the note

```case
tests/code_actions/fix/03_note_fixes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Formatting

Generated text is formatted with the project's clang-format style when one applies to the file, so a definition or a block of `case` labels lands in the surrounding code's layout.

<!-- BEGIN GENERATED ITEMS: code_actions/formatting -->

<!-- BEGIN CAPABILITY: supported -->

**Project formatting style**

Generated code follows the clang-format style that applies to the file

Without a `.clang-format` above the file, or with `DisableFormat`, the text keeps the layout shown in the other examples.

```case
tests/code_actions/formatting/01_project_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Known Limitations

- A definition placed in the host source goes after the last definition of the class's members the index knows in that file, or at the end of the file when it holds none. Which source file hosts a header follows the header's compilation context.
- Members already defined in another source file are left out of a "define missing members" action only when the project index knows that definition; with indexing disabled every undefined member is offered.
- A return type naming a member of a dependent base class is copied as written into an out-of-line definition, where it may need `typename` and the base's qualifier.
- Missing-include candidates come from the standard library mapping and from headers the project index has seen; a header no indexed source file includes is not suggested.

## Not Implemented

Extract function and variable, inline function and variable, moving a definition between header and source, converting an unscoped enum to a scoped one, and changing a function's signature across its callers.
