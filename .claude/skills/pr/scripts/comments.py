"""Print a PR's review findings without the bot boilerplate.

Unresolved review threads (every page) and the findings that bots post in
review bodies instead of inline threads, each reduced to author, location,
severity and the comment text. Badges, AI prompts, walkthroughs, tracking
comments and reaction footers are dropped.

    python3 comments.py [PR] [--all] [--max-chars N]
"""

import argparse
import json
import re
import subprocess
import sys
import time

BOTS = {"chatgpt-codex-connector", "coderabbitai", "github-actions"}

# <details> blocks whose summary matches are dropped with their content;
# every other block is unwrapped so nested findings stay visible.
NOISE_DETAILS = re.compile(
    r"Prompt for|About Codex|Walkthrough|Review details|Review info|Run configuration|Autofix"
    r"|Commits|Configuration used|Files selected|Files ignored|Files skipped|Additional context"
    r"|Additional comments|Learnings|Tips|Support|Docs|Pre-merge checks|Finishing touches"
    r"|Comment @coderabbitai",
    re.I,
)

THREADS_QUERY = """
query($owner: String!, $name: String!, $pr: Int!, $after: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $pr) {
      title
      reviewThreads(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id isResolved isOutdated path line originalLine
          comments(first: 30) { nodes { author { login } body url } }
        }
      }
    }
  }
}
"""

REVIEWS_QUERY = """
query($owner: String!, $name: String!, $pr: Int!) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $pr) {
      reviews(last: 50) { nodes { author { login } state body url commit { abbreviatedOid } } }
    }
  }
}
"""


def gh(*args):
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(result.stderr.strip() or f"gh {' '.join(args)} failed")
    time.sleep(1)
    return json.loads(result.stdout)


def graphql(query, **variables):
    args = ["api", "graphql", "-f", f"query={query}"]
    for key, value in variables.items():
        if value is not None:
            args += ["-F" if isinstance(value, int) else "-f", f"{key}={value}"]
    return gh(*args)["data"]["repository"]["pullRequest"]


def repo():
    info = gh("repo", "view", "--json", "owner,name")
    return info["owner"]["login"], info["name"]


def current_pr():
    return gh("pr", "view", "--json", "number")["number"]


def fetch_threads(owner, name, pr):
    threads, after, title = [], None, ""
    while True:
        page = graphql(THREADS_QUERY, owner=owner, name=name, pr=pr, after=after)
        title = page["title"]
        threads += page["reviewThreads"]["nodes"]
        info = page["reviewThreads"]["pageInfo"]
        if not info["hasNextPage"]:
            return title, threads
        after = info["endCursor"]


def fetch_reviews(owner, name, pr):
    return graphql(REVIEWS_QUERY, owner=owner, name=name, pr=pr)["reviews"]["nodes"]


def strip_details(text):
    def replace(match):
        summary = re.search(r"<summary>(.*?)</summary>", match.group(0), re.S)
        if summary and NOISE_DETAILS.search(summary.group(1)):
            return ""
        body = re.sub(r"<summary>(.*?)</summary>", r"\1\n", match.group(0), flags=re.S)
        return re.sub(r"</?details>", "", body)

    # Innermost blocks first so nested <details> unwrap correctly.
    pattern = re.compile(r"<details>(?:(?!<details>).)*?</details>", re.S)
    while pattern.search(text):
        text = pattern.sub(replace, text)
    return text


def clean(body):
    """Reduce a comment or review body to its findings text.

    Returns (severity, text); severity is the codex badge level (P1..P3) or
    the CodeRabbit level (Critical/Major/Minor) when one is present.
    """
    severity = ""
    if match := re.search(r"!\[(P\d) Badge\]", body):
        severity = match.group(1)
    elif match := re.search(r"_[^_\n]*?(Critical|Major|Minor|Trivial)_", body):
        severity = match.group(1)
    text = re.sub(r"<!--.*?-->", "", body, flags=re.S)
    text = strip_details(text)
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)
    text = re.sub(
        r"https://github\.com/[^/\s]+/[^/\s]+/blob/[0-9a-f]+/([^\s#]+)#L(\d+)(?:-L(\d+))?",
        lambda m: f"{m.group(1)}:{m.group(2)}"
        + (f"-{m.group(3)}" if m.group(3) else ""),
        text,
    )
    text = re.sub(r"<[^>\n]+>", "", text)
    text = re.sub(r"^(> ?)+", "", text, flags=re.M)
    text = re.sub(
        r"^\[!(?:CAUTION|NOTE|WARNING|TIP|IMPORTANT)\]\s*$", "", text, flags=re.M
    )
    text = re.sub(r"^\*\*[ \t]+", "**", text, flags=re.M)
    noise = re.compile(
        r"Useful\? React with|Codex Review$|automated review suggestions|Reviewed commit:|^-{3,}$"
    )
    lines = [
        line.rstrip() for line in text.splitlines() if not noise.search(line.strip())
    ]
    text = "\n".join(lines).strip()
    text = re.sub(r"\n{3,}", "\n\n", text)
    return severity, text


def is_finding(login, cleaned):
    """A bot review body counts when it carries a located finding, not a
    per-commit "reviewed" notice or an all-clear."""
    if login not in BOTS:
        return bool(cleaned)
    return bool(re.search(r"\S+:\d+|Outside diff range|Nitpick", cleaned))


def truncate(text, limit):
    return text if len(text) <= limit else text[: limit - 1].rstrip() + "…"


def indent(text, prefix="    "):
    return "\n".join(prefix + line if line else "" for line in text.splitlines())


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "pr", nargs="?", type=int, help="PR number (default: the current branch's PR)"
    )
    parser.add_argument("--all", action="store_true", help="include resolved threads")
    parser.add_argument(
        "--max-chars",
        type=int,
        default=1500,
        help="truncate each comment to this length",
    )
    args = parser.parse_args()

    owner, name = repo()
    pr = args.pr or current_pr()
    title, threads = fetch_threads(owner, name, pr)
    reviews = fetch_reviews(owner, name, pr)

    shown = [t for t in threads if args.all or not t["isResolved"]]
    unresolved = sum(1 for t in threads if not t["isResolved"])
    print(f"PR #{pr} {title}")
    print(f"review threads: {unresolved} unresolved of {len(threads)}")

    for index, thread in enumerate(shown, 1):
        comments = thread["comments"]["nodes"]
        if not comments:
            continue
        first, *replies = comments
        severity, text = clean(first["body"])
        location = f"{thread['path']}:{thread['line'] or thread['originalLine'] or '?'}"
        flags = " ".join(
            flag
            for flag, on in (
                ("resolved", thread["isResolved"]),
                ("outdated", thread["isOutdated"]),
                (severity, severity),
            )
            if on
        )
        print(
            f"\n[{index}] {thread['id']}  {location}  {first['author']['login']}  {flags}".rstrip()
        )
        print(indent(truncate(text, args.max_chars)))
        for reply in replies:
            _, reply_text = clean(reply["body"])
            if reply_text:
                print(
                    indent(
                        f"↳ {reply['author']['login']}: {truncate(reply_text, args.max_chars // 2)}",
                        "    ",
                    )
                )

    findings = []
    for review in reviews:
        login = review["author"]["login"]
        severity, text = clean(review["body"])
        if is_finding(login, text):
            findings.append((review, severity, text))
    if findings:
        print("\nfindings in review bodies (no inline thread):")
        for review, severity, text in findings:
            commit = review["commit"]["abbreviatedOid"] if review["commit"] else "?"
            label = f"{review['author']['login']} @ {commit} {severity}".rstrip()
            print(f"\n- {label}  {review['url']}")
            print(indent(truncate(text, args.max_chars)))


if __name__ == "__main__":
    main()
