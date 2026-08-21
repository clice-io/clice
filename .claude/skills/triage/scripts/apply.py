import argparse
import json
import subprocess
import time
from pathlib import Path


def parse_numbers(text):
    return {int(n) for n in text.split(",")} if text else set()


def gh(*args):
    for attempt in range(3):
        result = subprocess.run(["gh", *args], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout
        if attempt < 2:
            time.sleep(5 * (attempt + 1))
    raise SystemExit(f"gh {' '.join(args)} failed: {result.stderr.strip()}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("validated", help="validated.json from validate.py")
    parser.add_argument("--repo", default="clice-io/clice")
    parser.add_argument(
        "--only",
        default="",
        help="comma-separated issue numbers to apply (default all)",
    )
    parser.add_argument(
        "--skip", default="", help="comma-separated issue numbers to skip"
    )
    parser.add_argument(
        "--retitle",
        default="",
        help="'all' or comma-separated issue numbers whose better_title to apply",
    )
    parser.add_argument(
        "--include-closed",
        action="store_true",
        help="also edit closed issues (one-off migrations)",
    )
    args = parser.parse_args()

    only = parse_numbers(args.only)
    skip = parse_numbers(args.skip)
    retitle_all = args.retitle == "all"
    retitle = set() if retitle_all else parse_numbers(args.retitle)

    snapdir = Path(args.validated).parent
    titles = json.loads((snapdir / "titles.json").read_text())
    states = json.loads((snapdir / "states.json").read_text())

    for verdict in json.load(open(args.validated)):
        num = verdict["issue"]
        if (only and num not in only) or num in skip:
            continue
        live = json.loads(
            gh(
                "issue",
                "view",
                str(num),
                "--repo",
                args.repo,
                "--json",
                "labels,state,title",
            )
        )
        closed = live["state"] != "OPEN"
        live_labels = {label["name"] for label in live["labels"]}
        # The verdict was produced for the snapshotted state — any drift
        # (state flip, or someone else stamping `triaged`) makes it stale.
        if live["state"] != states.get(str(num)):
            print(f"#{num} skipped: state changed since snapshot")
            time.sleep(1)
            continue
        if "triaged" in live_labels:
            print(f"#{num} skipped: triaged since snapshot")
            time.sleep(1)
            continue
        if closed and not args.include_closed:
            print(f"#{num} skipped: no longer open")
            time.sleep(1)
            continue
        marker = "needs-triage" in live_labels
        live_kinds = sorted(label for label in live_labels if label.startswith("kind:"))
        if not marker and len(live_kinds) > 1:
            print(f"#{num} skipped: multiple kinds {live_kinds} (resolve manually)")
            time.sleep(1)
            continue
        proposed_kinds = [
            label for label in verdict["labels"] if label.startswith("kind:")
        ]
        proposed_kind = proposed_kinds[0] if proposed_kinds else None

        add = [label for label in verdict["labels"] if label not in live_labels]
        remove = ["needs-triage"] if marker else []
        if closed:
            add = [label for label in add if not label.startswith("status:")]
            remove += sorted(
                label for label in live_labels if label.startswith("status:")
            )
        maintainer_kind_wins = False
        if marker:
            remove += [label for label in live_kinds if label != proposed_kind]
        elif live_kinds and proposed_kind and proposed_kind not in live_kinds:
            # status: labels are kind-conditional per rules.md, so a verdict
            # built on the rejected kind cannot vouch for them either.
            add = [label for label in add if not label.startswith(("kind:", "status:"))]
            maintainer_kind_wins = True
            print(f"#{num} maintainer kind {live_kinds} wins over {proposed_kind}")

        native = {"os:linux", "os:macos", "os:windows"}
        combined = live_labels | set(add)
        if "os:wsl" in combined and combined & native:
            print(f"#{num} skipped: os:wsl vs native os conflict (resolve manually)")
            time.sleep(1)
            continue

        if (retitle_all or num in retitle) and verdict.get("better_title"):
            if maintainer_kind_wins:
                print(f"#{num} retitle skipped: title type derives from dropped kind")
            elif live["title"] != titles.get(str(num)):
                print(f"#{num} retitle skipped: title changed since snapshot")
            else:
                gh(
                    "issue",
                    "edit",
                    str(num),
                    "--repo",
                    args.repo,
                    "--title",
                    verdict["better_title"],
                )
                print(f"#{num} title → {verdict['better_title']}")

        # `triaged` hides the issue from every future snapshot, so it gets its
        # own final mutation: if any earlier edit fails, the script aborts
        # before the marker lands and the issue stays visible for re-triage.
        if remove:
            gh(
                "issue",
                "edit",
                str(num),
                "--repo",
                args.repo,
                "--remove-label",
                ",".join(remove),
            )
        if add:
            gh(
                "issue",
                "edit",
                str(num),
                "--repo",
                args.repo,
                "--add-label",
                ",".join(add),
            )
        gh("issue", "edit", str(num), "--repo", args.repo, "--add-label", "triaged")
        print(f"#{num} +{add + ['triaged']}" + (f" -{remove}" if remove else ""))
        time.sleep(1)


main()
