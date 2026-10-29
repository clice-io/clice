---
name: cpp-style
description: Read before writing, modifying or reviewing any C++ code — redundancy (the rule we care most about), modules and files, errors and defense, async, logging, naming, words, templates, API preferences.
---

# clice C++ Coding Style

## Redundancy Is a Defect

This is the convention we care most about. Redundant code is not a style
nit — it actively misleads: every guard implies the guarded state can
occur, every branch implies it can be taken, every parameter implies a
caller needs it. When that implication is false, the reader wastes time
defending against ghosts.

- **Every branch must be reachable.** Before adding a guard or fallback,
  prove the state can actually occur — construct the input that hits it.
  If you can't, don't write it.
- **No speculative generality.** No parameters, options, hooks, or
  abstraction layers for hypothetical future callers. Add them when the
  second real caller arrives.
- **One way to do each thing.** Don't leave an old path alive next to its
  replacement "just in case" — migrate all callers and delete it in the
  same change.
- **Re-read after every change.** Edits leave residue: conditions that
  became constant, variables read once, branches that now collapse,
  `else` after `return`, a helper with one remaining caller. Fold them
  before you're done — simplification that removes a concept beats one
  that merely shortens lines.
- **Delete, don't comment out.** Git history is the archive.
- **One behavior difference is one options field.** Two behaviors of the
  same logic are selected by a boolean in an options struct
  (`SemanticsOptions{.all_files = true}`), not by a subclass, a dummy
  collaborator, or a parallel code path.
- **Cross-cutting invariants get a single writer.** Accounting, budgets,
  ownership and visibility guarantees live in one type whose methods are the
  only transitions and whose doc comment lists the invariants; before coding,
  grep every site the invariant touches and funnel them through one helper
  so a missing site is discoverable. Per-site patches never catch up with a
  reviewer who re-derives the invariant from the whole diff.

## Files & Organization

- clice is one C++20 module, `clice`. An interface is a partition,
  `<stem>.cppm` beside its source, named after its layer and then what it
  is — usually its path under `src/` (`module clice:vfs.path;`,
  `unittests.*` under `unittests/`); `pixi run check-layers` checks the
  layer. A part grown into a directory keeps its interface there under the
  part's own name (`feature/hover/hover.cppm` is `feature.hover`), beside
  its implementation units and the internal partitions they share
  (`feature.hover.card`). Sources are implementation units `.cpp`, entry
  points `.cc` (`clice.cc`, `src/driver/*.cc`).
- Every module unit but the primary interface (`src/module.cppm`, which
  exports nothing) opens with `module;` and `#include "support/prelude.h"`:
  it includes the generated `modules/prelude.h`, which imports the
  third-party module (`modules/`) and `std.compat` and replays their
  macros, and gives the namespaces clice uses everywhere their short names
  in `clice` — `ranges`, `views`, `ipc`, `lsp`, `protocol`, `codec`,
  `json`, `fs`, `sys`. Code inside `clice` writes those names (`ranges::sort`,
  `protocol::Range`), never `std::ranges::` or `kota::ipc::protocol::`, and
  no file declares them again. Then come the macro headers the unit uses,
  the module declaration, and one `import :<partition>;` per partition it
  names.
- `.h` is left for what a module cannot carry — the prelude, macros
  (`*.macros.h`) and platform headers — with `#pragma once`, never
  include guards.
- File names are `snake_case`.
- Every file sees the names of all it imports: a file-local helper can
  clash with a name of another partition or lose overload resolution and
  ADL to a library function (`llvm::join`) no header used to bring in.
- File-local helpers: a single one is `static`; a cluster of them goes in
  one anonymous namespace.

## Data & Types

- `struct` by default, even for types with methods. `class` only when
  there is a real invariant that private access protects.
- `enum class` with an explicit underlying type (e.g. `: std::uint8_t`);
  document each enumerator with `///` when its meaning is not obvious.
- Prefer designated initializers (`{.field = value}`) for aggregate
  construction.
- West const: `const T&`, never `T const&`.
- A class lays out its public members first, then its private member
  functions under one `private:`, then its data members under a second
  `private:` — functions and fields never interleave. Data members stand
  one blank line apart, each one's comment on the lines above it, never
  trailing it.

## Errors, Exceptions & Defense

- The project builds with `-fno-exceptions -fno-rtti`: no
  `throw`/`try`/`catch`, no `dynamic_cast`/`typeid`. Polymorphic
  inspection uses `llvm::isa<>` / `cast<>` / `dyn_cast<>`.
- Fallible synchronous operations return `std::expected<T, E>` with `E` =
  `std::string` or `std::error_code`; async code uses kota's outcome
  types. `llvm::Error` only where an LLVM API forces it.
- `assert` pins preconditions; impossible branches end in
  `std::unreachable()` — never `llvm_unreachable` (project convention).
- Invalid external input (broken source code, malformed requests)
  degrades gracefully — empty result, input passed through — it never
  crashes the server. But graceful degradation is not scattered null
  checks: per the redundancy rule, defend only against states that can
  occur. The Debug (ASan + assertions) test surface is the safety net.

## Error Handling (control flow)

- Scope an error variable with an `if` init-statement when nothing after
  the `if` needs it: `if (auto result = foo(); !result.has_value())`.
- A type with `operator bool` needs no condition: `if (auto err = foo())`,
  never `if (auto err = foo(); err)`.
- When the success path needs the variable, declare it as a plain local
  and return early on the error — never an init-statement that forces the
  success path into an `else`.

## Concurrency & Async

- Async code is kota coroutines (`kota::task`, `co_await`) — no callback
  style. A public interface may stay synchronous and drive a coroutine
  internally when the caller has no event loop (see `Toolchain`).
- Review coroutine code by interleavings, not function by function: list
  every `co_await` suspension point, the events that can fire while it is
  suspended (crash, respawn, dispatch, cancellation, another coroutine
  resuming), and trace the shared state through each chain; then check that
  what the frame captured before suspending is still valid when it resumes.
  Bugs here are always cross-function interleavings; a function-level read
  finds none of them.
- Cancellation flows down the ownership tree through `task_group::cancel()`;
  tokens and sources exist only to cross an ownership boundary — the serve
  mode shutdown source in `MasterServer`, the batch commands' in
  `Lifetime` (`sched/batch.cpp`). Graceful shutdown is not
  cancellation — kota has no shield, a cancel cascades all the way down — so
  a shutdown is a cancellable serving phase bounded by `with_token`, then a
  non-cancellable drain that joins each task in order.
- `cancellation_token::wait()` completes by cancelling itself. Awaited
  directly inside `when_any` it propagates that cancellation into the parent
  frame and skips everything after the `co_await`; wrap it in `with_token`
  (which catches internally) or add `.catch_cancel()`.

## Logging

- Log through the `LOG_TRACE` / `LOG_DEBUG` / `LOG_INFO` / `LOG_WARN` /
  `LOG_ERR` macros (`std::format` syntax, source location captured
  automatically); `logging::critical` logs and aborts. Never call spdlog
  directly, never print diagnostics to stdout/stderr.

## Naming Conventions

- **Variables, member fields, function names**: `snake_case`. Class member fields do NOT use any special suffix/prefix (no trailing `_`, no `m_` prefix).
- **Class names, template parameter names, enum names**: `PascalCase`. Exception: some class names also use `snake_case` — follow the existing style in the project.
- **Enum values**: `PascalCase`.
- **Acronyms stay uppercase in type names**: `PCHFamily`, `ASTFamily`, `TUIndex` — never `PcmFamily`, `AstFamily`.
- **A file-local type is named by its role in the file**, without the
  words of the file's own subject: in an anonymous namespace,
  `FoldingRangeCollector` in `folding_ranges.cpp` is `Builder`, `ScanOptions`
  in `scan.cc` is `Options`. A type that is the file's subject itself keeps
  its name (`Indexer` in `indexer.cpp`).
- **Nothing is a `Collector`**: the type producing the file's answer is a
  `Builder`; a visitor gathering nodes is a `<What>Finder`
  (`PackForwardFinder`), or is named by what it gathers (`SpelledNames`); a
  clang callback that only records what clang reports is a `<What>Recorder`
  (`CompilationUnit::DiagnosticRecorder`).
- **Overloads, not prefixes, tell one job's variants apart**: every
  conversion to the LSP form is a `to_protocol` overload (a second form of
  the same input is named by the form: `to_symbol_information`), and a feature's
  answer from the index is an overload of its answer from the AST
  (`semantic_tokens(SourceText&, …)` beside `semantic_tokens(CompilationUnit&,
…)`) — never `semantic_tokens_to_protocol` or `index_semantic_tokens`.
- Doc comments on declarations use `///`; the bar for when to write a
  comment at all is in CLAUDE.md.

## Words to Use and Avoid

Identifiers and comments use plain words: the word a newcomer would guess
for the thing, not a metaphor or a rare word that needs a glossary
(verdict, envelope, ledger). One word means one thing across the codebase —
before naming, grep whether the word already means something else nearby,
and don't split one concept over two words. A name that its own doc comment
has to correct is the wrong name.

These were renamed out of the codebase; don't bring them back:

| Avoid                                          | Say instead                                                                                                                                                                                                                                                   |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| row / rows                                     | an index record: `occurrence` / `relation`, else `entry`; a file's whole index data: `index` / `shard` / `variant`; a source line: `line`                                                                                                                     |
| verdict                                        | `result` / `decision` / `outcome`, whichever the site is                                                                                                                                                                                                      |
| claim                                          | one word per site: `take`, `acquire`, `reserve`, `cover`, `report`                                                                                                                                                                                            |
| envelope                                       | what it is: `TUIndexBlob`, the TU index, a container, a header                                                                                                                                                                                                |
| ledger                                         | `table` / `list`                                                                                                                                                                                                                                              |
| wire (the serialized form)                     | `serialized` / `encoded`; "in the protocol" for IPC                                                                                                                                                                                                           |
| settle                                         | `finish` / `become idle`                                                                                                                                                                                                                                      |
| flavor                                         | `kind` / `variant`                                                                                                                                                                                                                                            |
| land / landing (a result arriving)             | `commit` / `merge` / `finish`                                                                                                                                                                                                                                 |
| sweep                                          | `pass` / `run` / `scan`; for cleanup, `remove`                                                                                                                                                                                                                |
| tier (an encoding choice)                      | `encoding` (`MaskEncoding`); the search ranking's `NameRank::tier` stays                                                                                                                                                                                      |
| surface (verb), vouch for                      | `report` / `show`, `guarantee` / `confirm`                                                                                                                                                                                                                    |
| stamp outside `vfs`                            | `version` / `generation`                                                                                                                                                                                                                                      |
| collector                                      | `Builder` / `<What>Finder` / `<What>Recorder` (see Naming Conventions)                                                                                                                                                                                        |
| `Features` (the server's request entry points) | `Requests`: the server routes requests; the features are `src/feature`'s                                                                                                                                                                                      |
| projection                                     | what it is: a document's latest compile (`DocumentCompile`), a result computed from the index (the `SourceText` overloads of `semantic_tokens`), the LSP form; only the indexer's step from the semantic map to entries is a projection (`project_semantics`) |
| evidence                                       | what it is: a candidate and what pairs it (`CounterpartCandidate`)                                                                                                                                                                                            |
| `switch_source_header` (the feature)           | `counterparts`: the files a file pairs with; only the editor command keeps the action's name (`clice.switchSourceHeader`)                                                                                                                                     |

Domain words with one meaning each — use them only in that meaning:

- **spelling**: how a path is written — the anchored absolute path,
  `..` kept, handed only to the identity functions or to clang.
- **host**: the source file whose compile context a header borrows.
- **slot**: one worker's place in the worker pool.
- **stamp**: in `vfs` only, a file's change fingerprint (size + mtime +
  ctime).

## Templates, Type Traits & Concepts

- `std::remove_cvref_t<T>` only where `T` can be deduced as a reference:
  a forwarding reference `T&&`. `T`, `T&` and `const T&` parameters,
  class template parameters and return types never deduce one.
- A type trait is a variable template specialized directly
  (`template<> inline constexpr bool is_my_type_v<MyType> = true;`), never
  a variable template wrapping a class template's `::value`.
- A concept over a trait uses `T` as is (`concept MyTrait = is_my_type_v<T>;`)
  unless it deliberately sees through references and cv-qualifiers; the
  caller passes the right type.

## String Literals

- Prefer C++11 raw string literals `R"(...)"` over escaped strings. Avoid `\"`, `\\`, `\n` in string literals when a raw literal is cleaner.

## Style

- Prefer `[[maybe_unused]]` over `(void)` for intentionally unused variables or parameters.
- Arithmetic counters step with `x += 1` / `x -= 1`, in `for` heads too — never `++`/`--`; split fused forms like `if(++x > n)` into two statements. Iterators and other types without `+=` are exempt. This applies to code you write or touch, not as a repository-wide sweep.

## Modern C++ Usage

- Use C++20/23 APIs whenever possible. Do NOT use `<iostream>` facilities (`std::cout`, `std::cin`, `std::cerr`, etc.). Also do NOT use C-style I/O (`printf`, `fprintf`, etc.).
- Prefer `ranges` / `views` APIs over raw loops and traditional `<algorithm>` calls.
- Range helpers come from the standard library, never LLVM's `STLExtras`: `ranges::contains` not `llvm::is_contained` (a set or map uses its own `.contains()`), `ranges::any_of` / `find_if` / `sort` not `llvm::`'s, `views::enumerate` / `zip` / `reverse` / `transform` not `llvm::enumerate` / `zip` / `reverse` / `map_range`. Only `llvm::concat` (C++26 in the standard) and `llvm::interleave` stay.
- Prefer LLVM's efficient data structures (e.g., `llvm::SmallVector`, `llvm::DenseMap`, `llvm::StringMap`, `llvm::StringRef`) over their `std` counterparts when appropriate.
- The operating system is reached through kotatsu: paths, files and directories through `fs` (`kota::fs`), the environment, programs and processes through `sys` (`kota::sys`), never `llvm::sys` or `std::filesystem`. LLVM stays where an interface takes its types — `llvm::vfs` for clang's file manager, a `llvm::MemoryBuffer` clang or the index adopts, filled from an `fs::File` — and for what kotatsu does not do: crash stack traces.

## Parameter Passing Preferences

- For string parameters, prefer `llvm::StringRef` > `std::string_view` > `const std::string&`.
- For array/span parameters, prefer `llvm::ArrayRef` > `std::span` > `const std::vector&`.
