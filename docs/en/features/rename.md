# Rename

Renaming a symbol (`textDocument/rename`, `F2` in VS Code) changes every place that names it across the workspace, from the project index. [`clice refactor rename`](../cli/refactor.md) runs the same engine from the command line; its page details what a rename follows and what it refuses.

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/rename/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Edits

A rename edits the declarations, the definition and every use of the symbol, and renames the symbols that must keep its name.

<!-- BEGIN GENERATED ITEMS: rename/edits -->

<!-- BEGIN CAPABILITY: supported -->

**Rename across files**

Renaming a function edits its declaration, its definition and every call in the workspace; a spelling inside a macro is left alone and named in a message

```case
tests-next/rename/edits/01_across_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**What a rename follows**

Renaming a class renames its constructors and destructor, a template its specializations, a virtual function its overrides

```case
tests-next/rename/edits/03_followers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Refused Renames

A symbol that cannot be renamed, or a new name that does not fit, is refused with the reason.

<!-- BEGIN GENERATED ITEMS: rename/refusals -->

<!-- BEGIN CAPABILITY: supported -->

**Names a rename refuses**

Pressing rename on a macro, an operator or a symbol of the system headers says why it cannot be renamed; off any name, nothing happens

```case
tests-next/rename/refusals/01_unrenamable_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**New names refused**

A new name that is no identifier, a keyword, taken in the same scope or a macro is refused with the reason; overloading a function or taking a reserved name is done with a warning

```case
tests-next/rename/refusals/02_rejected_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
