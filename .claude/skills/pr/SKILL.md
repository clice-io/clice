---
name: pr
description: The full pipeline from "code is ready" to "ready to merge" — pre-push verification, three-way self-review, opening the PR, watching CI and review threads. Read BEFORE committing, opening a PR, or checking on an open one.
---

# PR Pipeline

The development flow is: branch off `main` → write code (discussing requirements with the maintainer) → self-review and fix → open the PR → watch CI and reviews until everything is handled → report that it is ready. **The maintainer merges — never merge yourself.**

## Branch

- Always branch from freshly fetched `origin/main` — `git fetch origin && git checkout -b <type>/<short-topic> origin/main` — never from the local `main`. `<type>` uses the conventional-commit types, e.g. `fix/hover-crash`, `chore/upgrade-llvm-23`.
- An unpublished branch stacked on a predecessor that has since been squash-merged is rebased with `git rebase --onto origin/main <predecessor-tip>` before anything else (once the branch is an open PR, that rebase needs the maintainer's approval like any other) — a plain `git rebase origin/main` replays the predecessor's commits one at a time and stops on a conflict wherever their intermediate states differ from the squashed result. Never start a build or a suite on a tree with unresolved conflicts.

## Pre-push verification (every push, not just the first)

Never push anything unverified — "it compiles" is not verified, and CI is not a debugger.

1. **Docs sync — once, at the end, delegated.** If the branch touched snap fixtures, config annotations, or anything under `docs/`, the generated regions and the zh translations must be brought back in step before pushing. Do not chase them while developing: batch the whole sync here, and hand it to a subagent following the docs skill's "Syncing docs at the end of a branch" recipe so the report output and page texts stay out of the main context. Repeat on later pushes only when those inputs changed again.
2. `pixi run format`.
3. `npm run check` at the repo root when TypeScript changed.
4. All four test suites pass locally (the test skill). Every failure on the branch is yours to fix now — even if it looks pre-existing (main is green), and never by skipping, disabling, or weakening the test.
5. Read results from a full log, not a pipe: `cmd > /tmp/x.log 2>&1`, then the exit code and the log tail (`ninja: build stopped`, `FAILED`, the suite summary). `| tail` reports tail's exit code (a failed build looks green), `pipefail` + `| grep error` fails when there is nothing to find. Before running a suite, check the test binary is newer than your last source edit — a "1362 passed" on a stale binary verifies nothing.
6. Long local runs (build, suites, codex) go through Bash `run_in_background`, which notifies on exit — not a detached script polled by timed wake-ups.
7. A platform failure you can reproduce locally is fixed locally. With a Windows checkout reachable from WSL, build and run the failing suite there before pushing; CI is not a debugger.

## Pushing

`git status -sb` immediately before every push, then push with no arguments (upstream) or the branch name you just read. Never take the branch name from the session's start-of-conversation snapshot — it is stale after compaction, and a stale explicit name once resurrected a merged PR's deleted head branch (`git push origin :<branch>` undoes that).

## Self-review (before opening)

Commit all work first — the review diff only sees commits, so a dirty worktree means the reviewer inspects an incomplete patch (`git status` must be clean). Then review the full diff (`git diff origin/main...HEAD` — never the local `main`, which goes stale) and fix everything confirmed before opening:

1. **Primary: codex review** (the codex skill) — the canonical rules-aware review command: plain `codex exec` with gpt-6-astra at xhigh and a prompt that reads `.claude/CLAUDE.md` plus the cpp-style skill and reviews `git diff origin/main...HEAD` (the built-in `codex exec review --base` cannot take a prompt, so it never sees the repo rules). Treat findings as hypotheses: verify each concrete claim before fixing it.
2. **Supplement** — when codex is unavailable, or as an extra pass on high-risk diffs: 3 parallel subagents reviewing independently — correctness (logic errors, edge cases, undefined behavior), style (naming, cpp-style skill rules), tests (new functionality covered, no existing tests broken or weakened).

## Opening

- Confirm with the maintainer before creating the PR.
- One PR is one complete, user-perceivable capability. Implementation stages order the work and isolate risk; they are commit layering, not PR boundaries — the maintainer reviews one capability at a time, not a chain of partial PRs.
- Title follows the conventional commit format — CI checks it, and it becomes the squash-merge commit on `main`.
- Body follows `.github/pull_request_template.md`. Never reference local file paths, private notes, internal stage names or ledger numbers ("PR 2", "batch C", bug-list ids), or other material a reader without this machine cannot see.
- After `gh pr edit --body-file`, read the body back and check its length: an API timeout in a `gh pr view | gh pr edit` chain writes an empty body. Never pipe the two in one command.
- An external contributor's PR gets its title and body rewritten before merge — the title is the squash commit, and the body must describe the post-review state, including fixes made on their behalf.

## Watching

This is a sustained loop, not a single check — reviews and CI both take time, and a PR typically needs 5-6 rounds of check-and-fix before it is truly settled. Do not stop at the first green check.

- Cadence: one check every ~30 minutes by default, via timed wake-ups — never background shell loops. Tune to the situation: CI runs take their own time, and agent reviewers need a while to post after each push. Decide the total number of rounds yourself based on elapsed time and remaining activity.
- Every check covers BOTH: CI status AND unresolved review threads. A green pipeline with open review comments is not done. `python3 .claude/skills/pr/scripts/info.py [N]` prints the PR's state, merge status, check buckets (failing ones with links) and review load on one screen; `comments.py` prints the findings themselves.
- Bots also post findings in review bodies — the codex connector puts P2 findings there with blob permalinks, not as inline threads. `comments.py` lists both channels; when the maintainer reports a comment you cannot see as a thread, it is in a review body.
- Threads go through the resolve-comments skill: it pulls unresolved threads (by `isResolved`, never timestamps), applies root-cause fixes in the worktree, resolves the threads, and returns a compact summary — the GraphQL plumbing and comment bodies stay out of the main conversation. Threads are settled by resolving, not replying, and none stay open waiting for the maintainer: debatable points get the most defensible solution applied and recorded, batched into the final report.
- If resolve-comments left changes in the worktree: run the pre-push verification, then push an ordinary commit — never `--amend`, never force push. History rewrites destroy review anchors and reviewers' incremental diffs.
- Force push has essentially no legitimate use on a PR branch. The usual temptation — amending fixes into the previous commit to keep the branch "one tidy commit" — buys nothing: the squash merge flattens the branch anyway and only the PR title lands in `main` history. The single real case is rebasing onto a newer `main` (e.g. to resolve a conflict), and that requires asking the maintainer first.
- CI failure: reproduce and fix locally, verify, then push. Digest CI logs via a subagent; don't pull raw logs into the main conversation. Infrastructure failures — a runner that lost communication, a step that timed out on `npm install` — are not yours to fix: wait for the run to finish, then `gh run rerun <run-id> --failed` (non-interactive `gh` refuses to pick the run itself).
- Keep API calls sparse — one batch per check, `sleep 1` between `gh` calls.

## Done

- CI fully green and zero unresolved review threads → report to the maintainer that the PR is ready to merge: a one-paragraph summary of what review found and how it was addressed, plus every design decision taken while resolving threads (chosen vs. alternative), so the maintainer can accept or overturn them in one pass. Then stop — merging is the maintainer's call.
- The default bar stays high: elegant code, edge cases considered. "Merge now, clean up in a dedicated refactor PR later" is a real move in this project, but that trade-off is the maintainer's call, made case by case — never the agent's default.
