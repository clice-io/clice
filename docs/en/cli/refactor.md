# Refactor

## Overview

`clice refactor` rewrites the workspace's sources from the persisted index, the same index `clice query` reads. Nothing is compiled, so a refactoring over a large tree takes about as long as a query. `rename` is the one refactoring today; an editor's rename runs the same engine.

**Usage**: `clice refactor rename (--symbol <locator> --to <name> | --spec <file>) [--workspace <dir>] [--configuration <tag>] [--dry-run] [--fresh]`

## Rename

The symbol is named by a [locator](./query.md#locators), as `clice query` takes one: by its name, spelled exactly, case included, by the `symbol` an answer of `clice query` carried, or by a place, `file:line[:column]`. A near match or a pattern names nothing to rewrite a workspace for. A name the overloads of one scope spell renames them all; the `symbol` of one of them renames that one. A name several other symbols spell lists them as `candidates`.

A rename changes every declaration, definition and reference the index ties to the symbol, and the symbols that must keep its name:

- a class's constructors and destructor — renaming a constructor renames its class;
- the explicit and partial specializations of a template, and the deduction guides of a class template;
- the overrides a virtual function is linked with, in base and derived classes.

Operators, conversion functions, user-defined literals, macros, symbols declared in system headers and symbols whose every declaration a macro spells cannot be renamed. The new name must be an identifier that is a keyword in neither C nor C++; a name reserved for the implementation (`__x`, `_X`, and `_x` at global scope) is a warning.

## Many renames at once

`--spec <file>` makes every rename a file lists in one run: the index is read and the workspace swept once, so a terminology cleanup across hundreds of names costs about what one rename does, and the index is brought up to date once afterwards instead of between renames. `-` reads the list from stdin. Each line is `<locator> => <new name>`; blank lines and lines starting with `//` are skipped. A line naming several symbols is an error of its own, so a list is best written from the `symbol` each answer of `clice query` carries:

```
// row → variant, flavor → kind
#645572aaecc8aada => VariantHash
::clice::index::RowSource => IndexSource
JoinFlavor => JoinKind
```

The renames apply at once, each locator naming a symbol as the index has it before any of them: `A => B` beside `B => C` renames the old `B` to `C`, and two names may swap. Each new name is checked against the names the whole list leaves — a symbol another line renames away no longer collides with it, two lines giving symbols of one scope the same name do. A token naming two renamed symbols, such as a call in a template left open between two overloads, is changed when both get the same name and stops the list when they do not. A line repeating a rename another line makes is planned once.

The list is all or nothing: a line that names no symbol, a conflict of any line, or a stale file stops the whole list, and nothing is written. Fix the list and run it again; the index still matches the disk, so no reindex is needed between attempts.

## Answers

The answer is one JSON object, `{"result": ..., "stale": [...]}`, like a query's, the same for one rename and for a list:

- `renames` has one entry per rename asked for, with its `line` in the list, its `locator` as written, the symbol it found — `name`, `kind`, `symbol` — its `newName` and how many tokens it `edits`. An `error` says why its locator names no symbol to rename, with `candidates` when it names several. Its `warnings` are made anyway: the new name overloads a function of the same scope, hides or collides with a local of the same function, or is declared in a function using the renamed name, where it would capture that use. Its `conflicts` stop it: the new name is declared in the same scope already, is a macro, names a parameter or label of the same function, a member of a base or derived class, the class the renamed member belongs to or a member of the renamed class; the rename would edit a file outside the workspace; two namespaces would merge.
- `files` lists the tokens replaced in each file, with a 1-based line and byte column and the `name` and `newName` each trades. `heuristic` marks one reached through a name the index resolved by heuristics — a call in a template whose callee depends on a template parameter — worth a look.
- `unconfirmed` lists the places spelling an old `name` that the renames leave alone because the index cannot say what they name: the body of a `#define`, a macro whose expansion spells the name, an inactive `#if` branch, a file no unit of the build compiles, a token that also names another symbol of the old name — under another build configuration, or as another overload a call in a template may pick. Read them before building.
- `conflicts` are those between renames: a token two of them would change to different names.
- `stale` lists the files the renames edit, or that spell an old or a new name, whose indexed rows are not current — changed since they were indexed, or never indexed while some unit of the build still is not. They stop the renames as well; `--fresh` reindexes them first.

The exit code is 0 when the renames were written, or planned under `--dry-run`, and 1 when an error, a conflict or a stale file stopped them. Options and a list that do not parse answer `{"error": "..."}` — the list's error names its line — with exit code 1, or 2 when the arguments themselves do not parse.

## Writing

Without `--dry-run` the command writes the files once every new text is computed: a file whose text changed since the plan was made stops the whole rename before anything is written. Each file is rewritten in place, keeping its byte order mark, line endings and permissions; a file that cannot be written — read-only, or a full disk — stops the rename there, and the error names the files already rewritten.

Only the workspace is edited: hidden directories, the cache directory and build trees (a directory holding `CMakeCache.txt` or `build.ninja`) are left out. Its sources and headers are searched for the old and new names by suffix; an edit the index places may land in a file of any suffix.

The command reads the disk; unsaved editor buffers are invisible to it. Save them first, or rename from the editor.

## In the editor

The editor's rename selects the name under the cursor, or says why the symbol there cannot be renamed. The edits it returns are versioned against the open buffers, which it reads instead of the disk. Conflicts and stale files answer an error naming them; warnings, the places left alone and the edits made through heuristics are shown in a message.

## Known Limitations

- The checks are static: a change of meaning the index cannot see — argument-dependent lookup finding another function, a member of the new name capturing a use inside its class, an overload that cannot coexist with the renamed function — goes unnoticed. Build after renaming.
- Macros cannot be renamed yet.
- An editor's rename edits through the index of the project serving the file; another project open in the same editor keeps its uses of the symbol.
