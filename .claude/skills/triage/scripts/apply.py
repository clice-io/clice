import argparse
import json
import subprocess
import time


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

    for verdict in json.load(open(args.validated)):
        num = verdict["issue"]
        if (only and num not in only) or num in skip:
            continue
        live = json.loads(
            gh("issue", "view", str(num), "--repo", args.repo, "--json", "labels,state")
        )
        time.sleep(1)
        if live["state"] != "OPEN":
            print(f"#{num} skipped: no longer open")
            continue
        live_labels = {l["name"] for l in live["labels"]}
        add = [l for l in verdict["add"] if l not in live_labels]
        if any(l.startswith("kind:") for l in live_labels):
            dropped = [l for l in add if l.startswith("kind:")]
            if dropped:
                add = [l for l in add if not l.startswith("kind:")]
                print(f"#{num} kind already set, not adding {dropped}")
        edit = []
        if add:
            edit += ["--add-label", ",".join(add)]
        if "needs-triage" in live_labels:
            edit += ["--remove-label", "needs-triage"]
        if edit:
            gh("issue", "edit", str(num), "--repo", args.repo, *edit)
            print(
                f"#{num} +{add}"
                + (" -needs-triage" if "--remove-label" in edit else "")
            )
            time.sleep(1)
        if (retitle_all or num in retitle) and verdict.get("better_title"):
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
