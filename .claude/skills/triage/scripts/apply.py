import argparse
import json
import subprocess
import time


def parse_numbers(text):
    return {int(n) for n in text.split(",")} if text else set()


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
        if verdict["add"]:
            subprocess.run(
                [
                    "gh",
                    "issue",
                    "edit",
                    str(num),
                    "--repo",
                    args.repo,
                    "--add-label",
                    ",".join(verdict["add"]),
                ],
                check=True,
            )
            print(f"#{num} +{verdict['add']}")
            time.sleep(1)
        if (retitle_all or num in retitle) and verdict.get("better_title"):
            subprocess.run(
                [
                    "gh",
                    "issue",
                    "edit",
                    str(num),
                    "--repo",
                    args.repo,
                    "--title",
                    verdict["better_title"],
                ],
                check=True,
            )
            print(f"#{num} title → {verdict['better_title']}")
            time.sleep(1)


main()
