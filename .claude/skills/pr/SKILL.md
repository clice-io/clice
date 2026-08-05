---
name: pr
description: The full pipeline from "code is ready" to "ready to merge" — pre-push verification, three-way self-review, opening the PR, watching CI and review threads. Read BEFORE committing, opening a PR, or checking on an open one.
---

# PR Pipeline

The development flow is: branch off `main` → write code (discussing requirements with the maintainer) → self-review and fix → open the PR → watch CI and reviews until everything is handled → report that it is ready. **The maintainer merges — never merge yourself.**

## Branch

- Always `git fetch origin` first and branch from `origin/main` — never from the local `main`, which goes stale and causes messy surprises.
- Name branches `<type>/<short-topic>` using the conventional-commit types, e.g. `fix/hover-crash`, `chore/upgrade-llvm-23`.

## Pre-push verification (every push, not just the first)

Never push anything unverified — "it compiles" is not verified, and CI is not a debugger.

1. `pixi run format`.
2. `npm run check` at the repo root when TypeScript changed — strict tsc + ESLint across all workspace packages, zero tolerance.
3. All four test suites pass locally (`/test`). Every failure on the branch is yours to fix now — even if it looks pre-existing (main is green), and never by skipping, disabling, or weakening the test.

## Self-review (before opening)

Launch **3 parallel subagents** to review the full diff (`git diff main...HEAD`) independently, and fix everything they report before opening:

1. **Correctness reviewer**: logic errors, edge cases, undefined behavior, off-by-one mistakes.
2. **Style reviewer**: naming conventions, coding style, cpp-style skill rules.
3. **Test reviewer**: new functionality has tests, edge cases are covered, no existing tests were broken or weakened.

## Opening

- Confirm with the maintainer before creating the PR.
- Title follows the conventional commit format — CI rejects it otherwise, and since PRs are squash-merged with the title as the final commit message, the title is what lands in `main` history.
- Body follows `.github/pull_request_template.md`. Never reference local file paths, private notes, or other material a reader without this machine cannot see.

## Watching

This is a sustained loop, not a single check — reviews and CI both take time, and a PR typically needs 5-6 rounds of check-and-fix before it is truly settled. Do not stop at the first green check.

- Cadence: one check every ~30 minutes by default, via timed wake-ups — never background shell loops. Tune to the situation: CI runs take their own time, and agent reviewers need a while to post after each push. Decide the total number of rounds yourself based on elapsed time and remaining activity.
- Every check covers BOTH: CI status AND unresolved review threads. A green pipeline with open review comments is not done.
- Threads go through the resolve-comments skill: it pulls unresolved threads (by `isResolved`, never timestamps), applies fixes in the worktree, resolves the threads, and returns a compact summary — the GraphQL plumbing and comment bodies stay out of the main conversation. Threads are settled by resolving, not replying; only ones needing a maintainer decision stay open.
- If resolve-comments left changes in the worktree: run the pre-push verification, then push an ordinary commit — never `--amend`, never force push, never a rebase without asking. History rewrites destroy review anchors and reviewers' incremental diffs.
- CI failure: reproduce and fix locally, verify, then push. Digest CI logs via a subagent; don't pull raw logs into the main conversation.
- Keep API calls sparse — one batch per check, `sleep 1` between `gh` calls.

## Done

- CI fully green and zero unresolved review threads → report to the maintainer that the PR is ready to merge, with a one-paragraph summary of what review found and how it was addressed. Then stop — merging is the maintainer's call.
