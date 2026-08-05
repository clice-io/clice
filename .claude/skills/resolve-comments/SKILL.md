---
name: resolve-comments
description: Pull unresolved review threads of the current PR, apply the fixes in the worktree, resolve the threads, and return a compact summary. Runs in a forked context so the GraphQL plumbing and comment bodies never touch the main conversation.
context: fork
---

Handle one round of review comments for the current branch's PR.

## Fetch

Discover the PR number with `gh pr view --json number`, then pull the
threads (space `gh` calls with `sleep 1` — API rate limits are a real
concern):

```bash
gh api graphql -f query='
query($owner: String!, $repo: String!, $pr: Int!) {
  repository(owner: $owner, name: $repo) {
    pullRequest(number: $pr) {
      reviewThreads(first: 100) {
        nodes {
          id
          isResolved
          isOutdated
          path
          line
          comments(first: 10) { nodes { author { login } body } }
        }
      }
    }
  }
}' -F owner=clice-io -F repo=clice -F pr=<N> \
  --jq '.data.repository.pullRequest.reviewThreads.nodes | map(select(.isResolved | not))'
```

Always select by `isResolved == false` — never filter by timestamps.

## Handle each thread

- Valid point: apply the fix in the worktree. Do NOT commit or push —
  the main conversation runs the pre-push verification and pushes.
- Wrong, already addressed, or out of scope: no change.
- Needs a real design decision: leave the thread unresolved and flag it
  in the report.

## Resolve

Threads are settled by resolving, not replying — replies burn context
and review time. Resolve every thread you handled (fixed or judged
no-change); only flagged decision threads stay open:

```bash
gh api graphql -f query='
mutation($id: ID!) {
  resolveReviewThread(input: { threadId: $id }) { thread { id isResolved } }
}' -F id=<THREAD_ID>
```

## Report

One line per thread: `path:line — <the point, in a few words> — fixed in
<files> | no change (<why>) | NEEDS DECISION (<question>)`. End with
counts: threads fetched / fixed / resolved without change / left open,
and whether the worktree now has changes to verify and push.
