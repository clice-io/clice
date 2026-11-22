# Opening a PR

## Self-review

Commit all work first (`git status` clean): the reviewers read `git diff origin/main...HEAD` — never the local `main`, which goes stale.

1. **Opus reviewers, in parallel.** One Opus subagent (`model: "opus"`) per dimension, all launched in one message:
   - correctness — logic errors, edge cases, undefined behavior, coroutine interleavings (cpp-style, "Concurrency & Async");
   - project rules — CLAUDE.md and the cpp-style skill (write-tests for test code);
   - tests — new behavior covered, no existing test broken, skipped or weakened;
   - docs — only when the branch touched `docs/` or documented cases.

   Each prompt names its dimension, the diff command and the rule files to read, has the changed files read in full with the callers and callees of what changed — not only the diff's lines — and ends with the same fixed text: _Report only defects that lead to a concrete failure scenario — the input or state, and the wrong result it produces — each with file:line. Nothing outside this diff's scope. No refactoring, restructuring or style-preference suggestions. No findings is a valid answer._

2. **External agents, in addition — each only where it works.** In the background, beside the Opus reviewers, every agent of the external-agents skill gets its review prompt; one not installed, not logged in or out of quota is skipped without mention.
3. Every finding is a hypothesis: confirm it with a probe that fails on the defect before fixing it. Fix everything confirmed and commit; the checks then run on the fixed tree.

## The PR

- One PR is one complete, user-perceivable capability. Implementation stages order the work and isolate risk; they are commit layering, not PR boundaries.
- The title follows the commit format of the pull-request skill.
- The body follows `.github/pull_request_template.md`. "Tests" holds what CI does not show: the platforms and builds run outside it, an ecosystem run, measured numbers. Each push rewrites the body to the branch's final state.
- Never reference local file paths, private notes, internal stage names or ledger numbers ("PR 2", "batch C", bug-list ids), or other material a reader without this machine cannot see.
- After `gh pr edit --body-file`, read the body back and check its length: an API timeout in a `gh pr view | gh pr edit` chain writes an empty body. Never pipe the two in one command.
- An external contributor's PR gets its title and body rewritten before merge — the title is the squash commit's subject, and the body must describe the post-review state, including fixes made on their behalf.
