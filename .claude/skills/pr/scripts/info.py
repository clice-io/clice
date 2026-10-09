"""Print one screen of PR state: metadata, merge state, checks, review load.

python3 info.py [PR] [--commits N]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from comments import (
    clean,
    current_pr,
    fetch_reviews,
    fetch_threads,
    gh,
    is_finding,
    login,
    repo,
)  # noqa: E402

VIEW_FIELDS = (
    "number,title,state,isDraft,url,headRefName,baseRefName,headRefOid,author,mergeable,"
    "mergeStateStatus,reviewDecision,additions,deletions,changedFiles,updatedAt,labels,commits"
)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "pr", nargs="?", type=int, help="PR number (default: the current branch's PR)"
    )
    parser.add_argument(
        "--commits", type=int, default=5, help="how many recent commits to list"
    )
    args = parser.parse_args()

    pr = args.pr or current_pr()
    view = gh("pr", "view", str(pr), "--json", VIEW_FIELDS)
    # Exit code 8 means checks are pending; the JSON is printed either way.
    checks = gh(
        "pr", "checks", str(pr), "--json", "name,bucket,link", status_codes=(8,)
    )
    owner, name = repo()
    _, threads = fetch_threads(owner, name, pr)
    reviews = fetch_reviews(owner, name, pr)

    state = view["state"] + (" (draft)" if view["isDraft"] else "")
    labels = ", ".join(label["name"] for label in view["labels"])
    print(f"#{view['number']} {view['title']}")
    print(
        f"{state} · {view['headRefName']} → {view['baseRefName']} · {view['headRefOid'][:8]}"
        f" · by {login(view)} · +{view['additions']} -{view['deletions']} in {view['changedFiles']} files"
    )
    print(
        f"merge: {view['mergeable']} / {view['mergeStateStatus']} · review: {view['reviewDecision'] or 'none'}"
        f" · updated {view['updatedAt']}" + (f" · labels: {labels}" if labels else "")
    )

    buckets = {}
    for check in checks:
        buckets.setdefault(check["bucket"], []).append(check)
    summary = " · ".join(
        f"{len(buckets[b])} {b}"
        for b in ("pass", "fail", "pending", "skipping", "cancel")
        if b in buckets
    )
    print(f"checks: {summary or 'none'}")
    for check in buckets.get("fail", []) + buckets.get("cancel", []):
        print(f"  {check['bucket']:<8}{check['name']}  {check['link']}")
    if pending := buckets.get("pending"):
        print(f"  pending  {', '.join(check['name'] for check in pending)}")

    unresolved = sum(1 for thread in threads if not thread["isResolved"])
    findings = sum(
        1 for review in reviews if is_finding(review, clean(review["body"])[1])
    )
    print(
        f"review threads: {unresolved} unresolved of {len(threads)} · findings in review bodies: {findings}"
    )

    print("last commits:")
    for commit in view["commits"][-args.commits :]:
        print(f"  {commit['oid'][:8]} {commit['messageHeadline']}")
    print(view["url"])


if __name__ == "__main__":
    main()
