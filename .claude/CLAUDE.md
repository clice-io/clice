# clice — Project Guide

clice is a next-generation C++ language server (LSP) built on LLVM/Clang, targeting modern C++ (C++20/23). It uses a multi-process architecture: a master server coordinates stateless and stateful workers.

Detailed knowledge lives in skills — load them at the moments their descriptions state, don't work from memory:

- **cpp-style** — before writing or modifying any C++ code.
- **write-tests** — before writing or modifying anything under `tests/` (fixture forms, snapshot workflow and rules, pitfalls).
- **pr** — before committing, opening a PR, or checking on an open one (pre-push checks, self-review, CI/review watching).
- **resolve-comments** — each watch round of an open PR: pulls unresolved review threads, fixes and resolves them, returns a summary.
- **build / test / format** — build the project, run suites, format sources.
- **release / upgrade-llvm** — release operations and LLVM upgrades.

## Hard Rules

- **Never skip, disable, or weaken a failing test** — no `continue-on-error`, no expected-failure markers, no retries around flakiness. Fix the root cause. A failure on your branch is yours to fix now, even if it looks pre-existing (main is green), and never deferred to a follow-up PR.
- **Never push unverified code.** Build and run the relevant tests locally before every push; "it compiles" is not verified. Never debug by pushing attempts at CI.
- **Never rewrite history on an open PR branch** — no `--amend`, no force push, no rebase without asking first. Append ordinary commits; squash merge cleans up.
- **Never create or push tags, trigger release workflows, or publish anything** without explicit maintainer approval. Never change repository settings, rulesets, or branch protection.
- **Merging is the maintainer's call.** When a PR is green and all review threads are handled, report that it is ready — do not merge it yourself.

## Working Style

Distilled from real correction history — these mistakes keep recurring:

- **Read before you write.** The project already has a `Lexer`, `PositionMapper`, `CompilationUnitRef` helpers, `SemanticVisitor`, the `Tester` framework, and utilities in `src/support/` — search before adding anything that feels generic. New features copy the structure of 2-3 existing features of the same kind. When unsure whether to extend or create, ask.
- **Optimize the real scenario.** First ask when the code actually runs and what the user experiences (e.g. server startup is always a cold start — hot-cache numbers there are meaningless).
- **Refactoring means improving the abstraction**, not moving code. Understand what design problem the refactor solves before touching anything.
- **Comments talk to the future reader, not the diff reviewer.** The default for any change is zero new comments. Add one only when the final code, read on its own, would trap or mislead a competent reader — a non-obvious why, an invariant nothing enforces, a workaround for an external quirk that cost real debugging. Never comment what the code does, why the change is correct, or what was there before: that belongs in the PR description and becomes noise the moment it merges. Litmus test: cover the comment; if the surrounding code already tells the reader everything it said, delete it. When moving or rewriting code, existing explanatory comments DO survive — this rule is about adding, not preserving.
- **Apply cleanup and style instructions project-wide.** Grep for every occurrence and fix them in one pass, not just the file under discussion.
- **Calibrate confirmation to impact.** Small reversible changes: just do them. Architecture decisions, API changes, large refactors: propose first. Creating PRs, modifying CI: confirm. Once told "go ahead", execute fully without re-asking mid-way — follow-up pushes inside an approved PR flow are covered by the go-ahead.

## Source Layout

- `src/server/` — LSP server core: master server, compiler, indexer, stateful/stateless workers
- `src/feature/` — LSP feature implementations: hover, completion, document links, semantic tokens, etc.
- `src/compile/` — Compilation orchestration: compilation unit, directives, diagnostics
- `src/index/` — Symbol indexing: TUIndex, ProjectIndex, MergedIndex, include graph
- `src/semantic/` — Semantic analysis: symbol kinds, relations, AST visitor, template resolver
- `src/syntax/` — Lexer, scanner, token types, dependency graph
- `src/command/` — CLI parsing, compilation database, toolchain detection
- `src/driver/` — CLI subcommand entry points: serve, worker, index, inspect, format, lint, query, doc
- `src/support/` — Utilities: logging, filesystem, JSON, string helpers

Beyond `src/`: `tools/` is the TypeScript harness (`@clice/tools`: LSP client, snap machinery, replay, shared protocol types), `tests/` holds all four test suites, `editors/` the vscode/zed/nvim clients. `tools/`, `tests/`, and `editors/vscode` form one npm workspace rooted at the repo top level — run `npm install` and `npm run check` from the root, never inside a package.

## Build & Test

- **pixi** for environments, **CMake + Ninja** for building. Build types `Debug` and `RelWithDebInfo` (default); output in `build/[type]/`.
- Four test suites, all must pass before any push:
  - **Unit** (`tests/unit/`): C++, project's own framework. Test names at most 4 words.
  - **Integration** (`tests/integration/`): TypeScript vitest against a real clice server over LSP.
  - **Smoke** (`tests/smoke/`): recorded LSP sessions replayed via `tools/replay.ts`.
  - **Snap** (`tests/snap/`): feature snapshot corpora, pinned from both the standalone (`clice inspect`) and wire (real server) paths. A shared-snapshot mismatch between the two paths is a real bug — never `UPDATE_SNAPSHOTS` over it. Ownership rules and fixture meta live in the write-tests skill.
- TypeScript gate: `npm run check` at the repo root — strict tsc + ESLint across all workspace packages, zero tolerance.

## Commits, Branches, PRs

- **Conventional commits**, enforced by CI: `<type>(<scope>): <short description>`, subject under 70 characters.
  - Types: `feat`, `fix`, `refactor`, `chore`, `build`, `ci`, `docs`, `test`, `perf`, `style`, `revert`
  - Scopes: `src/` subdirectories or feature names, e.g. `completion`, `server`, `index`, `tests`, `document links`
- CI checks the head commit message on pushes and the **PR title** on pull requests. PRs are squash-merged with the title as the final commit message — the title is what lands in `main` history; individual branch commits matter less.
- Branches: `<type>/<short-topic>`, always created from freshly fetched `origin/main` — never from the local `main`, which goes stale and causes messy surprises.
- The whole pipeline from "code is ready" to "ready to merge" is the **pr** skill — follow it step by step.
