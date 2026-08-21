import argparse
import json
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

CHUNK = 25
BODY_LIMIT = 8000
COMMENT_LIMIT = 3000


def gh(*args):
    for attempt in range(3):
        result = subprocess.run(["gh", *args], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout
        time.sleep(5 * (attempt + 1))
    raise SystemExit(f"gh {' '.join(args)} failed: {result.stderr.strip()}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default="clice-io/clice")
    parser.add_argument("--out", default="/tmp/clice-triage")
    parser.add_argument(
        "--limit", type=int, default=0, help="cap untriaged issues (0 = all)"
    )
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    all_open = json.loads(
        gh(
            "issue",
            "list",
            "--repo",
            args.repo,
            "--state",
            "open",
            "--limit",
            "1000",
            "--json",
            "number,title,labels,createdAt,updatedAt",
        )
    )
    untriaged = [
        i
        for i in all_open
        if not any(l["name"].startswith("kind:") for l in i["labels"])
    ]
    if args.limit:
        untriaged = untriaged[: args.limit]

    now = datetime.now(timezone.utc)

    def age(iso):
        return (now - datetime.fromisoformat(iso.replace("Z", "+00:00"))).days

    waiting = ("status:needs-info", "status:needs-repro")
    digest = {
        "open_total": len(all_open),
        "untriaged": sorted(i["number"] for i in untriaged),
        "new_last_7d": [
            [i["number"], i["title"]] for i in all_open if age(i["createdAt"]) <= 7
        ],
        "stale_waiting": [
            [i["number"], i["title"], age(i["updatedAt"])]
            for i in all_open
            if any(l["name"] in waiting for l in i["labels"])
            and age(i["updatedAt"]) > 14
        ],
    }
    (out / "digest.json").write_text(json.dumps(digest, indent=1))

    existing = {}
    for pos, issue in enumerate(untriaged):
        chunk_dir = out / "issues" / f"chunk-{pos // CHUNK + 1}"
        chunk_dir.mkdir(parents=True, exist_ok=True)
        detail = json.loads(
            gh(
                "issue",
                "view",
                str(issue["number"]),
                "--repo",
                args.repo,
                "--json",
                "number,title,body,author,comments",
            )
        )
        author = (detail["author"] or {}).get("login", "ghost")
        labels = sorted(l["name"] for l in issue["labels"])
        lines = [
            f"# Issue #{detail['number']}: {detail['title']}",
            f"Author: {author}",
            f"Existing labels: {', '.join(labels) or '(none)'}",
            "",
            (detail["body"] or "(no body)")[:BODY_LIMIT],
        ]
        for comment in detail["comments"][:8]:
            who = (comment["author"] or {}).get("login", "ghost")
            lines += ["", f"--- comment by {who} ---", comment["body"][:COMMENT_LIMIT]]
        (chunk_dir / f"{detail['number']}.md").write_text("\n".join(lines))
        existing[str(detail["number"])] = labels
        time.sleep(1)
    (out / "existing-labels.json").write_text(json.dumps(existing, indent=1))

    chunks = (
        sorted(p.name for p in (out / "issues").glob("chunk-*")) if untriaged else []
    )
    print(f"untriaged: {len(untriaged)} issue(s) in {len(chunks)} chunk(s)")
    for name in chunks:
        print(f"  {out / 'issues' / name}")
    print(
        f"digest: {len(digest['new_last_7d'])} new in 7d, "
        f"{len(digest['stale_waiting'])} stale waiting"
    )


main()
