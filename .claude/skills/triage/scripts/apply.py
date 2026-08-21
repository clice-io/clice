import argparse
import json
import subprocess
import time
from pathlib import Path


def parse_numbers(text):
    return {int(n) for n in text.split(",")} if text else set()


def gh(*args):
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"gh {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


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
    args = parser.parse_args()

    only = parse_numbers(args.only)
    skip = parse_numbers(args.skip)
    retitle_all = args.retitle == "all"
    retitle = set() if retitle_all else parse_numbers(args.retitle)

    titles = json.loads((Path(args.validated).parent / "titles.json").read_text())

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
        if live["state"] != "OPEN":
            print(f"#{num} skipped: no longer open")
            time.sleep(1)
            continue
        live_labels = {label["name"] for label in live["labels"]}
        marker = "needs-triage" in live_labels
        live_kinds = sorted(label for label in live_labels if label.startswith("kind:"))
        proposed_kinds = [
            label for label in verdict["labels"] if label.startswith("kind:")
        ]
        proposed_kind = proposed_kinds[0] if proposed_kinds else None

        add = [label for label in verdict["labels"] if label not in live_labels]
        remove = ["needs-triage"] if marker else []
        maintainer_kind_wins = False
        if marker:
            remove += [label for label in live_kinds if label != proposed_kind]
        elif live_kinds and proposed_kind and proposed_kind not in live_kinds:
            add = [label for label in add if not label.startswith("kind:")]
            maintainer_kind_wins = True
            print(f"#{num} maintainer kind {live_kinds} wins over {proposed_kind}")

        native = {"os:linux", "os:macos", "os:windows"}
        combined = live_labels | set(add)
        if "os:wsl" in combined and combined & native:
            print(f"#{num} skipped: os:wsl vs native os conflict (resolve manually)")
            time.sleep(1)
            continue

        edit = []
        if add:
            edit += ["--add-label", ",".join(add)]
        if remove:
            edit += ["--remove-label", ",".join(remove)]
        if edit:
            gh("issue", "edit", str(num), "--repo", args.repo, *edit)
            print(f"#{num} +{add}" + (f" -{remove}" if remove else ""))

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
        time.sleep(1)


main()
