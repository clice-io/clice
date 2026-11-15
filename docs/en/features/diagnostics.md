# Diagnostics

## Core

Diagnostics are clang's errors and warnings on an open file, clang-tidy's findings, and clice's own messages about the setup.

- A client that pulls diagnostics (`textDocument/diagnostic`) asks for them; any other client gets them pushed when a compile finishes.
- Every missing `#include` is reported, not just the first, and the code after it is still checked.
- Warnings raised by directives in the preamble (`#warning`, `#pragma message`, macro redefinitions) show like any other.
- A compile command clang rejects before parsing (an unknown target) shows clang's reason at the top of the file.
- Errors of template instantiations inside headers show once the background index has compiled the file, and at once in a module unit or a file importing a module ([clangd#137](https://github.com/clangd/clangd/issues/137)).

## Errors in Included Headers

An error inside a header is reported on the `#include` that brings it in, so it shows in the file you are editing.

<!-- BEGIN GENERATED ITEMS: diagnostics/headers -->

<!-- BEGIN CAPABILITY: supported -->

**Errors in included headers**

An error inside an included header shows on the line that includes it

The first error of each included header is reported on its `#include` line, with a note at the place in the header where it occurred. The header's other errors belong to the header and show when it is opened.

```case
tests/diagnostics/headers/01_errors_on_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Errors in headers' function bodies**

An error inside a function body of a header shows once the background index has compiled the file as saved

The editor's compile of a file skips the bodies of the functions in the headers it includes at its top, as clangd does: the file opens and rebuilds after an edit several times faster. The background index compiles those bodies, and what it finds in them, an inline function's errors, a template's for the arguments the file instantiates it with, or those of the file's own generic lambda such a template calls, shows on the file while the file holds the text it compiled: an edit takes them down until the file is saved and compiled again. A rule setting `skip_function_body = false` for a header parses its bodies in the editor too; one setting `skip_function_body = true`, for third-party code, skips them in the background index as well, and what goes wrong in them never shows.

```case
tests/diagnostics/headers/09_skipped_bodies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Template Instantiations

An error or a warning inside a template is reported on the code that instantiates it.

<!-- BEGIN GENERATED ITEMS: diagnostics/templates -->

<!-- BEGIN CAPABILITY: supported -->

**Errors in template instantiations**

An error inside a template shows where the code asks for the instantiation

When a template fails for the arguments it is instantiated with, the error is reported on the code that requested the instantiation, not inside the template. Its notes lead through the chain of instantiations to the place in the template where it failed. A template in a header the file includes at its top is instantiated by the background index, not by the editor's compile, which skips the header's function bodies: its errors show once the index has compiled the file as saved.

```case
tests/diagnostics/templates/01_instantiation_errors
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Notes

The notes clang attaches to an error travel with it as related information; a client that takes no related information gets them in the message.

<!-- BEGIN GENERATED ITEMS: diagnostics/notes -->

<!-- BEGIN CAPABILITY: supported -->

**Notes as related information**

The notes clang attaches to an error point to the places they name

An error comes with the notes that explain it, such as where a name was first defined or which candidate a call could not use. Each note is related information of the error, so an editor shows it beside the error and jumps to its place.

```case
tests/diagnostics/notes/01_related_information
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Files Without a Compilation Database

A file the compilation database does not list still compiles, with a command clice guesses.

<!-- BEGIN GENERATED ITEMS: diagnostics/guidance -->

<!-- BEGIN CAPABILITY: supported -->

**No compilation database**

A file no compilation database lists compiles with a guessed command and says so

Without a `compile_commands.json` entry, a file still compiles, with a command clice guesses. A project without any database is told so in a message the editor shows, and when the guess leaves an include unresolved, a warning at the top of the file explains why and links to the setup guide. Once the database lists the file, the warning goes, and the errors left are the code's own.

```case
tests/diagnostics/guidance/01_no_database
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Tags

- `Deprecated` marks `-Wdeprecated` diagnostics and clang-tidy findings about deprecated headers and aliases.
- `Unnecessary` marks unused variable and parameter warnings and `misc-unused-*` findings.

## Publishing

- A client that takes only pushed diagnostics gets them when a compile of the file finishes, and anew when it saves an edited file; closing the file clears them.
- A saved header, or an edited `.clang-tidy`, brings the open files it affects up to date: a client that pulls diagnostics is asked to pull them again — Neovim 0.10 and 0.11 take no such request and show them at their next pull, after an edit — and the others get them recompiled and pushed. Diagnostics follow headers as saved, not their unsaved buffers, as in clangd.
- In VS Code, Problems lists a file's diagnostics only while an editor or a tab shows the file, even when another extension keeps the file open after it is closed.
- Each diagnostic carries a `code` — a warning's option (`-Wunused-variable`), clang's name for any other diagnostic, the clang-tidy check, or a name of clice's own (`module-not-built`, `inferred-compile-command`) — and its `source`: clang, clang-tidy or clice. A clang-tidy finding links to its check's documentation, a worker crash to clice's issue tracker.

## Diagnostic Suppression

A diagnostic clang gets wrong can be hidden by its code in the configuration, and comments silence the lines they are on.

<!-- BEGIN GENERATED ITEMS: diagnostics/suppression -->

<!-- BEGIN CAPABILITY: supported -->

**Suppressing diagnostics**

`suppress` hides diagnostics by their code

A diagnostic clang gets wrong on some code can be hidden by the code it shows with: a compiler warning's option, clang's name for any other diagnostic, or a clang-tidy check. The file's other features work as before, and `clice lint` still reports it.

```case
tests/diagnostics/suppression/01_by_code
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

`// NOLINT`, `// NOLINTNEXTLINE` and `// NOLINTBEGIN` / `// NOLINTEND` comments silence clang-tidy's findings on their lines, and the compiler's warnings too while clang-tidy runs on the file.

## Diagnostic Actions

- The fixes the compiler and clang-tidy attach to their diagnostics are offered as quick fixes.
- A clang-tidy fix is cleaned up as `clang-tidy --fix` cleans it: no `,` or `:` left dangling by what it removes.
- An include a fix adds to a module unit goes into its global module fragment, opened once.

## Header Diagnostics

- A `static inline` function a header defines and the file does not call raises no `-Wunused-function` ([clangd#1211](https://github.com/clangd/clangd/issues/1211)).
- A declaration used only inside a header's function body, which the editor skips, raises no false `-Wunused-function` or `-Wunused-member-function`: the background index reports the ones that are unused ([clangd#363](https://github.com/clangd/clangd/issues/363), [clangd#636](https://github.com/clangd/clangd/issues/636)).

## clang-tidy Integration

clang-tidy runs on open files with the checks the project's `.clang-tidy` enables, and its findings show beside the compiler's.

<!-- BEGIN GENERATED ITEMS: diagnostics/clang_tidy -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy findings**

clang-tidy runs on open files with the checks the nearest `.clang-tidy` enables

Each finding names its check and links to the check's documentation. The checks come from the nearest `.clang-tidy` above the file, as `clang-tidy` itself reads it.

```case
tests/diagnostics/clang_tidy/01_findings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Without a configuration file**

Without a `.clang-tidy`, a small set of checks runs

A project with no `.clang-tidy` still gets a few checks that rarely misfire, such as unused declarations, which an editor shows faded.

```case
tests/diagnostics/clang_tidy/02_default_checks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Checks left out**

A check too slow for an editor is named, not dropped silently

Open files run neither the checks too slow for an editor nor those unreliable on code being edited. clice names the slow ones the `.clang-tidy` enables once, with how to run them; `clice lint` runs both.

```case
tests/diagnostics/clang_tidy/03_editor_exclusions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Silencing with NOLINT**

`NOLINT` comments silence findings on their line, and compiler warnings too

`// NOLINT` silences its own line and `// NOLINTNEXTLINE` the next, for every check or the ones named in parentheses. While clang-tidy runs on the file, they silence compiler warnings on those lines as well.

```case
tests/diagnostics/clang_tidy/04_nolint
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Fixes as quick fixes**

A finding that carries a fix offers it as a quick fix

The fix a check attaches to its finding is offered among the quick fixes at the finding; applying it clears the finding.

```case
tests/diagnostics/clang_tidy/05_quick_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Turning clang-tidy off**

`clang_tidy = false` keeps clang-tidy off open files

With clang-tidy off, open files show only the compiler's diagnostics, whatever the `.clang-tidy` enables.

```case
tests/diagnostics/clang_tidy/09_turned_off
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Running the slow checks**

`slow_clang_tidy_checks = true` also runs the checks too slow for an editor

Checks too slow for an editor, such as `misc-const-correctness`, run on open files only when asked for: every compile of the file waits for them.

```case
tests/diagnostics/clang_tidy/20_slow_checks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- Checks too slow for an editor run on open files only when `[diagnostics] slow_clang_tidy_checks` asks for them, and unless it says `false` clice names the ones it leaves out; those unreliable on code being edited are always left out; `clice lint` runs them all.
- `performance-use-std-move` is left out of both the editor and `clice lint`: in the LLVM clice is built on, it crashes on classes read from a PCH or a module.
- `.clang-tidy` edits apply to open files at their next compile.
- A crash of a clang-tidy check pauses clang-tidy on that file, not its other features.
- clang-tidy's warnings that originate in macros of system headers are left out ([clangd#1587](https://github.com/clangd/clangd/issues/1587), [clangd#2000](https://github.com/clangd/clangd/issues/2000)).
- A malformed `.clang-tidy` is reported at the top of the files it configures, and runs no check ([clangd#2313](https://github.com/clangd/clangd/issues/2313)).
- `clice lint` lists the warnings of the whole project ([clangd#1973](https://github.com/clangd/clangd/issues/1973)).
