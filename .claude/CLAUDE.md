# clice — Project Guide

clice is a next-generation C++ language server (LSP) built on LLVM/Clang, targeting modern C++ (C++20/23). It uses a multi-process architecture: a master server coordinates stateless and stateful workers.

The detailed rules live in the skills under `.claude/skills/`; each skill's description says when to load it. Load it at that moment — don't work from memory.

## Hard Rules

- **Never skip, disable, or weaken a failing test** — no `continue-on-error`, no expected-failure markers, no retries around flakiness. Fix the root cause. A failure on your branch is yours to fix now, even if it looks pre-existing (main is green), and never deferred to a follow-up PR.
- **Never leave a push unverified.** Pushing and verifying run side by side: push without waiting, while a box (the build and test skills) and CI run the relevant build and tests; the work is done only when both are green, and "it compiles" is not verified. A failure is reproduced and fixed on a box, not by pushing guesses at CI.
- **Never rewrite history on an open PR branch** — no `--amend`, no force push, no rebase without asking first. Append ordinary commits; squash merge cleans up.
- **Never create or push tags, trigger release workflows, or publish anything** without explicit maintainer approval. Never change repository settings, rulesets, or branch protection.
- **Never merge without the maintainer's explicit authorization for that PR.** Approval of the plan or of another PR does not carry over. A PR that is green with every review thread handled is reported as ready, then left alone.

## Working Style

Distilled from real correction history — these mistakes keep recurring:

- **Read before you write.** The project already has a `Lexer`, `PositionMap`, `CompilationUnit` helpers, `SemanticVisitor`, the `Tester` framework, and utilities in `src/support/` — search before adding anything that feels generic. New features copy the structure of 2-3 existing features of the same kind. When unsure whether to extend or create, ask.
- **Understand and design yourself.** Read the code in order and think in this conversation. A subagent is for a narrow factual lookup — one or two at most — never for a judgment or a design. Mechanical batches (migrations, scans, multi-dimension review, the docs sync) go to Opus subagents (`model: "opus"`) whose prompt fixes the spec and the acceptance criteria.
- **Optimize the real scenario.** First ask when the code actually runs and what the user experiences (e.g. server startup is always a cold start — hot-cache numbers there are meaningless).
- **Ask who hits a corner case, and how often.** A rare state a real workflow reaches gets an explicit error that names it, never a silent fallback that guesses; a state no real workflow reaches gets no code and is reported as a known limitation. Broken source code and malformed requests are not corner cases: they are ordinary input and degrade gracefully (cpp-style).
- **Stay in scope.** A problem outside the task is reported, not fixed. A problem only Objective-C code reaches is always reported, never fixed.
- **Refactoring means improving the abstraction**, not moving code. Understand what design problem the refactor solves before touching anything.
- **Comments talk to the future reader, not the diff reviewer.** The default for any change is zero new comments. Add one only when the final code, read on its own, would trap or mislead a competent reader — a non-obvious why, an invariant nothing enforces, a workaround for an external quirk that cost real debugging. Never comment what the code does, why the change is correct, or what was there before: that belongs in the PR description and becomes noise the moment it merges. Litmus test: cover the comment; if the surrounding code already tells the reader everything it said, delete it. When moving or rewriting code, existing explanatory comments DO survive — this rule is about adding, not preserving.
- **Apply cleanup and style instructions project-wide.** Grep for every occurrence and fix them in one pass, not just the file under discussion.
- **Calibrate confirmation to impact.** Small reversible changes: just do them. Architecture decisions, API changes, large refactors: propose first. Creating PRs, modifying CI: confirm. Once told "go ahead", execute fully without re-asking mid-way — follow-up pushes inside an approved PR flow are covered by the go-ahead.

## Source Layout

`src/` is one C++20 module, `clice`, in layers that import only downward — core (`support` ← `vfs` ← `command` ← `syntax` ← `compile` ← `semantic` ← `index` ← `feature`) ← `config` ← {`project`, `worker`} ← `sched` ← `server`, `project` ← `analysis`, {`analysis`, `server`} ← `driver`. `pixi run check-layers` checks it, as CI does.

- `src/support/` — utilities: logging, JSON, text, processes, fuzzy matching
- `src/vfs/` — disk access: reads, statuses, writes, directory walks; the file table and its freshness checks; the on-disk cache store
- `src/command/` — compile commands: compiler argument parsing, the compilation database, toolchain queries, NVCC
- `src/syntax/` — lexer, scanner, tokens, include resolution, dependency graph
- `src/compile/` — compilation: compilation unit, tokens, directives, diagnostics, clang-tidy
- `src/semantic/` — semantic analysis over the AST: symbol kinds, relations, template resolver, declaration, type and expression helpers; the per-unit semantic map (AST visitor), selection, entity identities and doc comments
- `src/index/` — symbol indexing: TU index, project index, shards, the database, queries, rename
- `src/feature/` — LSP features: hover, completion, document links, semantic tokens, etc.
- `src/config/` — the `clice.toml` configuration
- `src/project/` — the loaded project: CDB loading and watching, command resolution, header hosting, the index store
- `src/worker/` — the stateless and stateful worker processes, their pool and protocol
- `src/sched/` — the task graph (PCH, PCM, AST and TU nodes), batch runs, the indexing pump
- `src/server/` — the LSP server: master server, sessions, dispatch, the control channel
- `src/analysis/` — the analyses behind `clice analyze` and `clice modularize`: module graph, rewriting, wrapping
- `src/driver/` — the CLI: `driver.cppm` and one self-registering file per subcommand (serve, worker, index, inspect, scan, format, lint, query, refactor, analyze, modularize)

Beyond `src/`: `tools/` is the TypeScript harness (`@clice/tools`: the case framework in `tools/testing`, LSP client, shared protocol types, docs and compat tooling), `unittests/` the unit tests, `tests/` the cases and, beside them, the compat (`tests/compat`) and Zed (`tests/editors/zed`) suites, `editors/` the vscode/zed/nvim clients. `modules/` is the generated module of the third-party headers: a header or macro clice newly needs goes into `modules/demand.cpp`, the rest is regenerated and never hand-edited (the upgrade-llvm skill, Step 4). `tools/`, `tests/` and `editors/vscode` form one npm workspace rooted at the repo top level — run `npm install` and `npm run check` from the root, never inside a package.

## Build & Test

- **pixi** for environments, **Bazel** for building (npm's bazelisk: `npx bazel`, available once `pixi run install-tests` has run). Build types `Debug` (ASan and assertions) and `RelWithDebInfo` (default), each in its own output directory: `pixi run build [type]` puts clice at `build/[type]/bin/bin/clice`.
- **Where builds run:** on a box (the build skill), not on this machine unless truly necessary — it hosts every agent's session, and one build saturates it. A session may hold several, one per kind of work (platforms, Debug, A/B). Pushing never waits for a box: CI has the capacity to verify in parallel.
- Two test suites, both must pass for every push, on the box and in CI: **unit** (`unittests/`, C++ on zest) and **cases** (`tests/`, every other test: one directory per case, run against a real server, `clice inspect` and the CLI; `tests/README.md` is the contract).
- **Compat** (`tests/compat/`): real build systems and compilers build a small project; clice must parse the resulting database clean and agree with the compiler. It needs those tools installed, so it runs in CI; run it on a box when changing command handling.
- TypeScript gate: `npm run check` at the repo root — strict tsc + ESLint across all workspace packages, zero tolerance.

## Branches and Commits

- Branches are `<type>/<short-topic>`, always created from freshly fetched `origin/main` (`git fetch origin && git switch -c <branch> origin/main`) — never from the local `main`, which goes stale.
- Commit with the repository's configured identity (`git config user.email`), never an email the session environment supplies.
- Commit and PR title format and everything from "code is ready" to "ready to merge" is the **pr** skill.
