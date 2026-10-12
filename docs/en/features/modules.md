# C++20 Modules

clice builds the modules a file imports when the file opens, so module units and their importers work like any other source: hover, navigation and completion reach across `import`. The feature pages show what each feature does in module units; this page shows the modules themselves.

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/modules/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Imports

A file that imports a module sees the names the module exports, and those of the modules it re-exports.

<!-- BEGIN GENERATED ITEMS: modules/imports -->

<!-- BEGIN CAPABILITY: supported -->

**Imported symbols**

A name imported from a module reads its declaration on hover, in a file that imports the module

The importing file compiles against the module's interface, which clice
builds when the file opens; nothing else needs to be open.

```case
tests-next/modules/imports/01_imported_symbol
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Re-exported modules**

A module that re-exports another with `export import` gives its importers the names of both

```case
tests-next/modules/imports/04_reexport
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Exports

A declaration is exported on its own, in an `export` block, or as a member of an `export namespace`.

<!-- BEGIN GENERATED ITEMS: modules/exports -->

<!-- BEGIN CAPABILITY: supported -->

**Export blocks and namespaces**

An `export { … }` block exports every declaration inside it, and `export namespace` every member of the namespace

```case
tests-next/modules/exports/01_export_forms
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Units

A module can be split into partitions, and a unit can include headers in its global module fragment.

<!-- BEGIN GENERATED ITEMS: modules/units -->

<!-- BEGIN CAPABILITY: supported -->

**Module partitions**

A module split into partitions gives its importers what every partition its primary interface re-exports declares

A partition imports a sibling by its partition name alone, `import :Types;`,
and the primary interface uses what its partitions declare.

```case
tests-next/modules/units/01_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Global module fragment**

A module unit includes headers in its global module fragment, ahead of the module declaration, and imports modules after it

```case
tests-next/modules/units/03_global_fragment
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Header units**

Importing a header as a header unit, `import "local.h";` or `import <util.h>;`, is not supported yet

```case
tests-next/modules/units/09_header_units
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Edits

An edit to a module interface reaches its importers once it is saved.

<!-- BEGIN GENERATED ITEMS: modules/edits -->

<!-- BEGIN CAPABILITY: supported -->

**Interface edits reach importers**

An importer sees what its module's interface declares once the interface is saved, not while the edit is unsaved

```case
tests-next/modules/edits/03_unsaved_interface
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Build Failures

A module that fails to build reports its errors, and the files importing it report the import.

<!-- BEGIN GENERATED ITEMS: modules/failures -->

<!-- BEGIN CAPABILITY: partial -->

**Broken modules**

A module that fails to build reports its own errors, and the files importing it report the import

```case
tests-next/modules/failures/01_broken_module
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
