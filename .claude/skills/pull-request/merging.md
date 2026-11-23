# Merging a PR

Only with the maintainer's authorization for this PR.

```bash
gh pr merge <N> --squash --match-head-commit <the head you verified> --body-file /tmp/merge-body-<N>.md
```

- The commit message is written for `git log`: 3–6 lines of plain prose, wrapped at 72 columns — what changes for the user, and why. No headings, lists, tables or test results; `(#N)` leads to the rest. Without `--body-file`, the message is the whole PR body.
- Without `--subject`, the subject is the PR title with ` (#N)`. A `--subject` replaces it whole and has to end in ` (#N)` itself.
