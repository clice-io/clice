# Refactor

## Overview

`clice refactor` rewrites the project's sources from the persisted index, the same index `clice query` reads. Nothing is compiled, so a refactoring over a large tree takes about as long as a query. `rename` is the one refactoring today; an editor's rename runs the same engine.

**Usage**: `clice refactor rename (<locator> <new-name> | --spec <file>) [--workspace <dir>] [--configuration <tag>] [--dry-run] [--fresh]`

## Rename

The symbol is named by a [locator](./query.md#locators), as `clice query` takes one: by its name, spelled exactly, case included, by the `symbol` an answer of `clice query` carried, or by a place, `file:line[:column]`. A near match or a pattern names nothing to rewrite a project for. A name the overloads of one scope spell renames them all; the `symbol` of one of them renames that one. A name several other symbols spell lists them as `candidates`.

A rename changes every declaration, definition and reference the index ties to the symbol, and the symbols that must keep its name:

- a class's constructors and destructor — renaming a constructor renames its class;
- the explicit and partial specializations of a template, and the deduction guides of a class template;
- the overrides a virtual function is linked with, in base and derived classes.

In a template, a name that depends on a template parameter (`value.print()`, `T::type`) is renamed where the instantiations the index holds resolve it to the symbol, and listed where none was indexed. Where one instantiation resolves it to the symbol and another to another symbol of the old name, the name can neither change nor stay: the rename stops, unless a list of renames gives both the same new name. A call whose overloads the template's own lookup finds (`compute(value)`) is renamed whenever the rename takes all of them, instantiated or not.

Operators, conversion functions, user-defined literals, symbols declared in system headers and symbols whose every declaration a macro spells cannot be renamed. The new name must be an identifier that is a keyword in neither C nor C++; a name reserved for the implementation (`__x`, `_X`, and `_x` at global scope) is a warning.

## Macros

A macro's name is one name in every unit that sees it, so renaming a macro renames the definitions of it that go together: every `#define` of it in a file defining or using it — each branch of an `#if`, a redefinition after an `#undef` — and the definitions a use of it resolves to elsewhere, such as a header compiled under other definitions, or a body that names a macro each file defines for it (`DEBUG_TYPE` and `LLVM_DEBUG`). Another library's macro of the same name stays as it is. The rename edits its uses, written or as another macro's argument, the `#ifdef`, `#ifndef` and `defined` that test it, its `#undef`s, the bodies of other macros that name it once expanded, and, in the files defining or using it, the directives naming it where no definition reaches: the `#ifndef` guarding a default or a header, a `#define` in a branch the build leaves out.

A macro the compile command or the compiler defines (`-DNAME`, `__GNUC__`), or that a system header defines, cannot be renamed, nor one a unit's command overrides at a use the rename would change. The new name of a macro must be spelled by no symbol, macro or `#ifdef` of the project, whatever its scope: the macro would replace it. `defined` cannot name a macro.

## Many renames at once

`--spec <file>` makes every rename a file lists in one run: the index is read once, so a terminology cleanup across hundreds of names costs about what one rename does, and the index is brought up to date once afterwards instead of between renames. `-` reads the list from stdin. Each line is `<locator> => <new name>`; blank lines and lines starting with `//` are skipped. A line naming several symbols is an error of its own, so a list is best written from the `symbol` each answer of `clice query` carries:

```
// row → variant, flavor → kind
#645572aaecc8aada => VariantHash
::clice::index::RowSource => IndexSource
JoinFlavor => JoinKind
```

The renames apply at once, each locator naming a symbol as the index has it before any of them: `A => B` beside `B => C` renames the old `B` to `C`, and two names may swap. Each new name is checked against the names the whole list leaves — a symbol another line renames away no longer collides with it, two lines giving symbols of one scope the same name do. A token naming two renamed symbols, such as a call in a template left open between two overloads, is changed when both get the same name and stops the list when they do not. A line renaming a symbol an earlier line renames already is planned once when it gives the same name, and is a conflict when it gives another.

The list is all or nothing: a line that names no symbol, a conflict of any line, or a stale file stops the whole list, and nothing is written. After a line naming no symbol or a conflict, fix the list and run it again: nothing was written, so the index still matches the disk. A stale file needs reindexing first, which `--fresh` does.

## Answers

The answer is one JSON object, `{"result": ..., "stale": [...]}`, like a query's, the same for one rename and for a list:

- `renames` has one entry per rename asked for, with its `line` in the list, its `locator` as written, the symbol it found — `name`, `kind`, `symbol` — its `newName` and how many tokens it `edits`. An `error` says why its locator names no symbol to rename, with `candidates` when it names several. Its `warnings` are made anyway: the new name overloads a function of the same scope, or is reserved for the implementation; some units of the build are not indexed yet, and what they name is not renamed. Its `conflicts` stop it: the new name is declared in the same scope already, is a macro, names any symbol or `#ifdef` of the project for a macro, names a parameter or label of the same function, a member of a base or derived class, the class the renamed member belongs to or a member of the renamed class; a token it changes also names another symbol of the old name in another instantiation or under another build configuration; the rename would edit a file outside the project; two namespaces would merge; an earlier line renames the symbol to another name; a token it changes also names a symbol another line renames to another name.
- `files` lists the tokens replaced in each file, with a 1-based line and byte column and the `name` and `newName` each trades.
- `unconfirmed` lists the places the index ties to an old `name` that the renames leave alone because no written code says what they name: the body of a `#define`, which names something else in another expansion, a macro whose expansion spells the name, or a macro name pasted together with `##`, a name in a template no instantiation the index holds resolves to the symbol, a call in a template that may pick another overload of the old name, and, for a macro, a directive naming it where no definition reaches in a file the macro is neither defined nor used in. Read them before building.
- `conflicts` stop all the renames together: a token two of them would change to different names, or a file that could not be rewritten.
- `stale` lists the files the renames edit, or that spell an old or a new name, whose text changed since they were indexed. They stop the renames as well; `--fresh` reindexes them first.

The exit code is 0 when the renames were written, or planned under `--dry-run`, and 1 when an error, a conflict or a stale file stopped them. Options and a list that do not parse answer `{"error": "..."}` — the list's error names its line — with exit code 1, or 2 when the arguments themselves do not parse.

## Writing

Without `--dry-run` the command writes the files once every new text is computed: a file whose text changed since the plan was made stops the whole rename before anything is written. Each file is rewritten in place, keeping its byte order mark, line endings and permissions; a file that cannot be written — read-only, or a full disk — stops the rename there, and the error names the files already rewritten.

Only the project is edited: hidden directories, the cache directory and build trees (a directory holding `CMakeCache.txt` or `build.ninja`) are left out; an edit the index places may land in a file of any suffix. A rename reads the index, the files it edits and the files changed since they were indexed, never the rest of the project: what the index ties to no symbol is neither renamed nor listed — the code an `#if` leaves out, the body of a `#define` no expansion runs, a file no unit of the build compiles or includes. Search the project for the old name after renaming.

The command reads the disk; unsaved editor buffers are invisible to it. Save them first, or rename from the editor.

## In the editor

The editor's rename selects the name under the cursor, or says why the symbol there cannot be renamed. On a name in a template that depends on a template parameter, it renames what the instantiations resolve the name to, and refuses when they resolve it to several symbols or none was indexed. The edits it returns are versioned against the open buffers, which it reads instead of the disk. Conflicts and stale files answer an error naming them; warnings and the places left alone are shown in a message.

## Examples

### Renaming a Symbol

A dry run shows the plan, and a run without it writes the files.

<!-- BEGIN GENERATED ITEMS: refactor/edits -->

<!-- BEGIN CAPABILITY: supported -->

**Plan, then write**

A dry run lists the edits without writing them, and a run without it rewrites the files

`--dry-run` answers with every name the rename would change, file by file, and
with the places it leaves alone for you to check: the body of `CALL` spells
`compute`, but a macro body names what each expansion makes of it. Without
`--dry-run` the files are rewritten. Renaming a class renames its constructors
with it.

```case
tests/refactor/edits/01_plan_then_write
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Ambiguous Names

A name several symbols spell renames nothing until a locator picks one.

<!-- BEGIN GENERATED ITEMS: refactor/locators -->

<!-- BEGIN CAPABILITY: supported -->

**Name an ambiguous symbol**

A name two symbols spell lists both, and a scoped name or a candidate's `symbol` picks one

Nothing is renamed while the name is ambiguous. Each candidate carries a
`symbol` to pass back as it is.

```case
tests/refactor/locators/02_ambiguous_name
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Many Renames

A list of renames is planned and written in one run.

<!-- BEGIN GENERATED ITEMS: refactor/lists -->

<!-- BEGIN CAPABILITY: supported -->

**Rename a list at once**

A file listing renames, one `<symbol> => <new name>` a line, makes them all in one run

Every line is planned against the same index and the files are written once,
all of the renames or none of them. Blank lines and lines starting with `//`
are skipped.

```case
tests/refactor/lists/01_rename_list
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Known Limitations

- The checks are static: a change of meaning the index cannot see — argument-dependent lookup finding another function, a member of the new name capturing a use inside its class, a local or a template parameter of the new name clashing with or hiding the renamed one inside a function, an overload that cannot coexist with the renamed function — goes unnoticed. Build after renaming.
- A system header reading a macro of the project (`NOMINMAX` for `windows.h`) stops its rename only when an indexed unit includes that header; a macro name a `#` turns into a string keeps its old spelling. A macro's new name is checked against the symbols a name search finds and those the edited files name: a local variable or a parameter of another file spelled so goes unnoticed.
- An editor's rename edits through the index of the project serving the file; another project open in the same editor keeps its uses of the symbol.
