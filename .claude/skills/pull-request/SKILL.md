---
name: pull-request
description: Read before committing, opening a PR, or checking on an open one — commit and title format, the checks of every push, self-review, opening, waiting for and reading CI (tools/github), handling review threads, merging.
---

# PR Pipeline

Branch (CLAUDE.md) → code → commit → self-review and fixes → checks, with the push and opening beside the build and tests → watch CI and threads until settled → report ready → merge once the maintainer authorizes this PR.

Each stage has its file beside this one, read when the stage starts:

- `opening.md` — before opening: the self-review, the title and the body.
- `watching.md` — each round on an open PR: waiting for CI and reading it, CI failures, the review threads.
- `merging.md` — once the maintainer authorized this PR's merge.

## Commits and titles

- Conventional commits, checked by CI: `<type>(<scope>): <short description>`, the scope optional, under 70 characters. Types: `feat`, `fix`, `refactor`, `chore`, `build`, `ci`, `docs`, `test`, `perf`, `style`, `revert`. Scopes: `src/` subdirectories or feature names, e.g. `completion`, `server`, `index`, `tests`, `document links`.
- A title names one thing: no `;` or `,`. A PR whose changes seem to need a list is titled by what they share, and the parts go in the body.
- CI checks the head commit message on pushes and the PR title on pull requests. A PR lands in `main` as one squash commit: the title is its subject, and its message is the PR body unless the merge writes its own (`merging.md`).
- An unpublished branch stacked on a predecessor that has since been squash-merged is rebased first, after `git fetch origin`, with `git rebase --onto origin/main <predecessor-tip>` — a plain `git rebase origin/main` replays the predecessor's commits one at a time and stops wherever their intermediate states differ from the squashed result. Never start a build or a suite on a tree with unresolved conflicts.

## Checks (every push, not just the first)

1. **Docs sync — once, at the end, delegated.** If the branch touched documented cases (a `README.md` or the output of a case that has one), config annotations, or anything under `docs/`, hand the sync to an Opus subagent following the write-docs skill's `sync.md`. Repeat on later pushes only when those inputs changed again.
2. `pixi run format`.
3. `npm run check` at the repo root when TypeScript changed; `pixi run check-layers` when C++ imports or includes changed; `pixi run check-timers` when C++ waits on the clock.
4. What the change runs (the build-and-test skill, "What a change runs") passes, beside CI: push without waiting for it.

## Pushing

`git status -sb` immediately before every push, then push with no arguments (upstream) or the branch name you just read. Never take the branch name from the session's start-of-conversation snapshot — it is stale after compaction, and a stale explicit name once resurrected a merged PR's deleted head branch (`git push origin :<branch>` undoes that).

## Done

- CI finished and passed (`ci.ts status`), zero unresolved threads, and every finding in review bodies handled → report to the maintainer that the PR is ready to merge: a one-paragraph summary of what review found and how it was addressed, plus every design decision taken while resolving threads (chosen vs. alternative), so the maintainer can accept or overturn them in one pass. Then stop.
- The default bar stays high: elegant code, edge cases considered. "Merge now, clean up in a dedicated refactor PR later" is a real move in this project, but that trade-off is the maintainer's call, made case by case — never the agent's default.
