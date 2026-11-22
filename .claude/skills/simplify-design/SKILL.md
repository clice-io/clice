---
name: simplify-design
description: Use when asked, or before opening a PR whose work grew fix by fix — guards, flags, states or parallel paths added one after another as each problem showed up: rethink the requirement from first principles, cut what it does not need, and rewrite the design to the simplest shape that meets it. Not for a small change, a clean staged implementation, or the review rounds of an open PR.
---

# Simplifying a design

A long run of fixes leaves its history in the code: a guard per failure met, a flag per case, a structure that served one step and outlived it, two paths where one would do. Each commit was reasonable; together they are not what someone who knew the final requirement would write. This pass writes that.

## 1. The requirement

- What the user — an editor, the CLI, an agent — sees when it works, in a few sentences, from the task and the conversation rather than from the code.
- The facts about clice the solution rests on, each checked in the code ("only units are indexed; a header through the units that read it").
- Why each piece was added: the commit messages, and the session's history where it is searchable (`search_history`); read them instead of guessing.

## 2. The design from scratch

Before reading the diff, sketch the smallest design that meets the requirement: its data, its invariants, the one flow through it. Reading the branch first anchors the design on its accidental structure.

## 3. What the branch added

From `git diff origin/main...HEAD`, list every mechanism the commits introduced — a type, a field, a parameter, an option, a branch, a cache, a retry, a fallback, a state, a wrapper that only passes things on. Each earns its place against the sketch, by the user-visible failure it answers and who reaches it (CLAUDE.md, "Ask who hits a corner case"; cpp-style, "Redundancy Is a Defect"). One that answers a reviewer's hypothetical or a step that is gone is deleted. One a real workflow reaches but not worth the cost becomes a `// LIMITATION:` only once the maintainer accepts it.

## 4. The structure

The directory test, before the rewrite and after it: every file and type the branch touched or added, each in one sentence — what it is, and why it stands alone. A sentence that needs "and" to join two things, a name that does not match the content, two files calling each other, a word with two meanings, a helper with one caller in a lower layer are structure problems, fixed in this pass.

## 5. The rewrite

- The new shape has fewer concepts — states, types, branches, layers — than the old one, not the same ones moved around. When the branch already is the simplest shape, the pass ends here, with no change.
- Behavior stays, and the tests are its contract: only a test of an internal the rewrite removed goes, and only while the behavior it reached stays covered by another; every other test passes unchanged.
- A small duplicate that only a new wrapper would remove stays.
- What CLAUDE.md's "Calibrate confirmation to impact" asks to propose first — a change users see, an interface other code depends on, an architecture decision — is proposed first, and so are a new `// LIMITATION:` and the deletion of a test.

## 6. The report

What was cut and why (the failure it answered, or that it answered none), what was restructured, every test deleted, and what users see differently — each with who reaches it. Then the pull-request skill, from its self-review on.
