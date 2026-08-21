import argparse
import json
import shutil
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

CHUNK = 25
BODY_LIMIT = 8000
COMMENT_LIMIT = 3000
COMMENT_WINDOW = 30
MAINTAINERS = {"16bit-ykiko"}


def gh(*args):
    for attempt in range(3):
        result = subprocess.run(["gh", *args], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout
        if attempt < 2:
            time.sleep(5 * (attempt + 1))
    raise SystemExit(f"gh {' '.join(args)} failed: {result.stderr.strip()}")


def login(entry):
    name = (entry or {}).get("login", "ghost")
    return f"{name} (maintainer)" if name in MAINTAINERS else name


def untriaged_filter(issue):
    return "triaged" not in {label["name"] for label in issue["labels"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default="clice-io/clice")
    parser.add_argument("--out", default="/tmp/clice-triage")
    parser.add_argument(
        "--limit", type=int, default=0, help="cap untriaged issues (0 = all)"
    )
    parser.add_argument(
        "--state",
        choices=("open", "all"),
        default="open",
        help="'all' sweeps closed issues too (one-off migrations)",
    )
    args = parser.parse_args()

    out = Path(args.out)
    shutil.rmtree(out / "issues", ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    for stale in (
        "existing-labels.json",
        "titles.json",
        "states.json",
        "validated.json",
        "digest.json",
    ):
        (out / stale).unlink(missing_ok=True)
    for stale_verdict in out.glob("verdicts-*.md"):
        stale_verdict.unlink()

    listed = json.loads(
        gh(
            "issue",
            "list",
            "--repo",
            args.repo,
            "--state",
            args.state,
            "--limit",
            "1000",
            "--json",
            "number,title,labels,state,createdAt,updatedAt",
        )
    )
    if len(listed) == 1000:
        print("warning: hit the 1000-issue listing cap, snapshot may be incomplete")
    all_open = [i for i in listed if i["state"] == "OPEN"]
    untriaged = [i for i in listed if untriaged_filter(i)]
    selected = untriaged[: args.limit] if args.limit else untriaged

    now = datetime.now(timezone.utc)

    def age_days(iso):
        return (
            now - datetime.fromisoformat(iso.replace("Z", "+00:00"))
        ).total_seconds() / 86400

    waiting = ("status:needs-info", "status:needs-repro")
    digest = {
        "open_total": len(all_open),
        "untriaged": sorted(i["number"] for i in untriaged),
        "new_last_7d": [
            [i["number"], i["title"]] for i in all_open if age_days(i["createdAt"]) <= 7
        ],
        "stale_waiting": [
            [i["number"], i["title"], int(age_days(i["updatedAt"]))]
            for i in all_open
            if any(label["name"] in waiting for label in i["labels"])
            and age_days(i["updatedAt"]) > 14
        ],
    }
    (out / "digest.json").write_text(json.dumps(digest, indent=1))

    existing = {}
    titles = {}
    states = {}
    for pos, issue in enumerate(selected):
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
        labels = sorted(label["name"] for label in issue["labels"])
        body = detail["body"] or "(no body)"
        if len(body) > BODY_LIMIT:
            body = body[:BODY_LIMIT] + "\n[... body truncated ...]"
        lines = [
            f"# Issue #{detail['number']}: {detail['title']}",
            f"State: {issue['state']}  Author: {login(detail['author'])}",
            f"Existing labels: {', '.join(labels) or '(none)'}",
            "",
            body,
        ]
        comments = detail["comments"]
        if len(comments) > COMMENT_WINDOW:
            half = COMMENT_WINDOW // 2
            omitted = len(comments) - 2 * half
            comments = comments[:half] + [None] + comments[-half:]
        for comment in comments:
            if comment is None:
                lines += ["", f"[... {omitted} comments omitted ...]"]
                continue
            lines += [
                "",
                f"--- comment by {login(comment['author'])} ---",
                comment["body"][:COMMENT_LIMIT],
            ]
        (chunk_dir / f"{detail['number']}.md").write_text("\n".join(lines))
        existing[str(detail["number"])] = labels
        titles[str(detail["number"])] = detail["title"]
        states[str(detail["number"])] = issue["state"]
        time.sleep(1)
    (out / "existing-labels.json").write_text(json.dumps(existing, indent=1))
    (out / "titles.json").write_text(json.dumps(titles, indent=1))
    (out / "states.json").write_text(json.dumps(states, indent=1))

    chunks = (
        sorted(p.name for p in (out / "issues").glob("chunk-*")) if selected else []
    )
    scope = (
        f"{len(selected)} of {len(untriaged)}"
        if len(selected) != len(untriaged)
        else f"{len(untriaged)}"
    )
    print(f"untriaged: {scope} issue(s) in {len(chunks)} chunk(s)")
    for name in chunks:
        print(f"  {out / 'issues' / name}")
    print(
        f"digest: {len(digest['new_last_7d'])} new in 7d, "
        f"{len(digest['stale_waiting'])} stale waiting"
    )


main()
