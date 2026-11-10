# Rename

Renaming a symbol (`textDocument/rename`, `F2` in VS Code) changes every place that names it across the workspace, from the project index. [`clice refactor rename`](../cli/refactor.md) runs the same engine from the command line; its page details what a rename follows and what it refuses.

<!-- The cards in the sections below are generated from the documented cases
     in tests/rename/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Edits

A rename edits the declarations, the definition and every use of the symbol, and renames the symbols that must keep its name.

<!-- BEGIN GENERATED ITEMS: rename/edits -->

<!-- BEGIN CAPABILITY: supported -->

**Rename across files**

Renaming a function edits its declaration, its definition and every call in the workspace; a spelling inside a macro is left alone and named in a message

```case
tests/rename/edits/01_across_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**What a rename follows**

Renaming a class renames its constructors and destructor, a template its specializations, a virtual function its overrides

```case
tests/rename/edits/03_followers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Rename a macro**

Renaming a macro edits its `#define` in each branch of the `#if` it is defined in, the `#ifndef` guarding a default, and every use, written as another macro's argument or in another macro's body; a use pasted together with `##` is left alone and named in a message

```case
tests/rename/edits/12_macros
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Rename a template's dependent name**

In a template, a call depending on a template parameter is renamed where the indexed instantiations resolve it to the renamed method; one another instantiation resolves to another method of the same name stops the rename, and one no instantiation resolves is left alone and named in a message

```case
tests/rename/edits/13_templates
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Objective-C names**

Renaming an Objective-C class, protocol, category or instance variable changes every place that writes it

```case
tests/rename/edits/14_objective_c
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Refused Renames

A symbol that cannot be renamed, or a new name that does not fit, is refused with the reason.

<!-- BEGIN GENERATED ITEMS: rename/refusals -->

<!-- BEGIN CAPABILITY: supported -->

**Names a rename refuses**

Pressing rename on an operator, a symbol or a macro of the system headers, a macro the compile command defines or a name its instantiations resolve to several symbols says why it cannot be renamed; off any name, nothing happens

```case
tests/rename/refusals/01_unrenamable_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**New names refused**

A new name that is no identifier, a keyword, taken in the same scope or a macro is refused with the reason; overloading a function or taking a reserved name is done with a warning

```case
tests/rename/refusals/02_rejected_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**New macro names refused**

A macro has no scope: a new name some symbol, macro or `#ifdef` of the project already spells is refused, as is `defined`; a reserved name is done with a warning

```case
tests/rename/refusals/05_macro_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Objective-C methods and properties**

Renaming an Objective-C method or property is refused, with the reason

A method's selector names every method its messages may reach, and a
property its accessors and the instance variable it synthesizes; an
instance variable a property synthesizes has no declaration to rename.

```case
tests/rename/refusals/07_objective_c
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
