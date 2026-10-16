---
name: pr
description: Read before committing, opening a PR, or checking on an open one — commit and title format, the checks of every push, self-review, opening, watching CI and review threads.
---

# PR Pipeline

Branch (CLAUDE.md) → code → commit → self-review and fixes → checks, with the push and opening beside the box run → watch CI and threads until settled → report ready. Merging needs the maintainer's explicit authorization for this PR.

## Commits and titles

- Conventional commits, checked by CI: `<type>(<scope>): <short description>`, subject under 70 characters. Types: `feat`, `fix`, `refactor`, `chore`, `build`, `ci`, `docs`, `test`, `perf`, `style`, `revert`. Scopes: `src/` subdirectories or feature names, e.g. `completion`, `server`, `index`, `tests`, `document links`.
- CI checks the head commit message on pushes and the PR title on pull requests. PRs are squash-merged with the title as the commit message, so the title is what lands in `main`.
- An unpublished branch stacked on a predecessor that has since been squash-merged is rebased first, after `git fetch origin`, with `git rebase --onto origin/main <predecessor-tip>` — a plain `git rebase origin/main` replays the predecessor's commits one at a time and stops wherever their intermediate states differ from the squashed result. Never start a build or a suite on a tree with unresolved conflicts.

## Checks (every push, not just the first)

1. **Docs sync — once, at the end, delegated.** If the branch touched documented cases (a `README.md` or the output of a case that has one), config annotations, or anything under `docs/`, hand the sync to an Opus subagent following the docs skill's "Syncing docs at the end of a branch". Repeat on later pushes only when those inputs changed again.
2. `pixi run format`.
3. `npm run check` at the repo root when TypeScript changed; `pixi run check-layers` when C++ imports or includes changed; `pixi run check-timers` when C++ waits on the clock.
4. The unit tests, the cases and `npm test -w tools` pass on a box (the test skill) and in CI. The two run side by side: push without waiting for the box. Changes to async code, shutdown or position math run the Debug suites too: ASan and the assertions of clang and kotatsu live only there.
5. Read results from a full log, not a pipe: `cmd > /tmp/x.log 2>&1`, then the exit code and the log tail (`Build did NOT complete successfully`, `FAILED`, the suite summary). `| tail` reports tail's exit code (a failed build looks green), `pipefail` + `| grep error` fails when there is nothing to find. Before running a suite, check the test binary was built after your last source edit (on a box: after your last `box push`) — a pass on a stale binary verifies nothing.
6. Long runs (`box run` builds and suites, codex) go through Bash `run_in_background`, which notifies on exit — not a detached script polled by timed wake-ups.
7. A platform failure is reproduced and fixed on a box of that OS (the test skill).

## Pushing

`git status -sb` immediately before every push, then push with no arguments (upstream) or the branch name you just read. Never take the branch name from the session's start-of-conversation snapshot — it is stale after compaction, and a stale explicit name once resurrected a merged PR's deleted head branch (`git push origin :<branch>` undoes that).

## Self-review (before opening)

Commit all work first (`git status` clean): the reviewers read `git diff origin/main...HEAD` — never the local `main`, which goes stale.

1. **Opus reviewers, in parallel.** One Opus subagent (`model: "opus"`) per dimension, all launched in one message:
   - correctness — logic errors, edge cases, undefined behavior, coroutine interleavings (cpp-style, "Concurrency & Async");
   - project rules — CLAUDE.md and the cpp-style skill (write-tests for test code);
   - tests — new behavior covered, no existing test broken, skipped or weakened;
   - docs — only when the branch touched `docs/` or documented cases.

   Each prompt names its dimension, the diff command and the rule files to read, and ends with the same fixed text: _Report only defects that lead to a concrete failure scenario — the input or state, and the wrong result it produces — each with file:line. Nothing outside this diff's scope. No refactoring, restructuring or style-preference suggestions. No findings is a valid answer._

2. **codex, in addition — only when it is logged in and has quota.** Run the codex skill's review command in the background beside the Opus reviewers. If `codex login status` fails or the run stops on a usage limit, skip it without mention.
3. Every finding is a hypothesis: confirm it with a probe that fails on the defect before fixing it. Fix everything confirmed and commit; the checks then run on the fixed tree.

## Opening

- One PR is one complete, user-perceivable capability. Implementation stages order the work and isolate risk; they are commit layering, not PR boundaries.
- The title follows the commit format above.
- The body follows `.github/pull_request_template.md`. Never reference local file paths, private notes, internal stage names or ledger numbers ("PR 2", "batch C", bug-list ids), or other material a reader without this machine cannot see.
- After `gh pr edit --body-file`, read the body back and check its length: an API timeout in a `gh pr view | gh pr edit` chain writes an empty body. Never pipe the two in one command.
- An external contributor's PR gets its title and body rewritten before merge — the title is the squash commit, and the body must describe the post-review state, including fixes made on their behalf.

## Watching

A sustained loop, not a single check — a PR typically needs 5-6 rounds of check-and-fix before it is settled. Do not stop at the first green check.

- Cadence: one check every ~30 minutes by default, via timed wake-ups — never background shell loops. CI runs take their own time, and agent reviewers need a while to post after each push; decide the number of rounds from elapsed time and remaining activity.
- Every check covers BOTH CI status AND unresolved review threads. `python3 .claude/skills/pr/scripts/info.py [N]` prints the PR's state, merge status, check buckets (failing ones with links) and review load on one screen; `comments.py` prints the findings themselves, including those bots post in review bodies instead of threads (the codex connector puts its P2 findings there).
- Threads go through the resolve-comments skill. If it left changes in the worktree: commit them as an ordinary commit and run the checks. A rebase onto a newer `main` (to resolve a conflict) is the only reason to rewrite a PR branch, and it needs the maintainer's approval first.
- CI failure: reproduce it on a box of the failing platform (the test skill), fix, verify, then push. Digest CI logs through an Opus subagent, not in the main conversation; `gh run view --log` refuses while the run is in progress, `gh api repos/clice-io/clice/actions/jobs/<id>/logs` reads a finished job of a running run. Infrastructure failures — a runner that lost communication, a step that timed out on `npm install` — are not yours to fix: wait for the run to finish, then `gh run rerun <run-id> --failed` (non-interactive `gh` refuses to pick the run itself).
- Keep API calls sparse — one batch per check, `sleep 1` between `gh` calls.

## Done

- CI fully green, zero unresolved review threads, and every finding bots posted in review bodies handled → report to the maintainer that the PR is ready to merge: a one-paragraph summary of what review found and how it was addressed, plus every design decision taken while resolving threads (chosen vs. alternative), so the maintainer can accept or overturn them in one pass. Then stop.
- The default bar stays high: elegant code, edge cases considered. "Merge now, clean up in a dedicated refactor PR later" is a real move in this project, but that trade-off is the maintainer's call, made case by case — never the agent's default.
