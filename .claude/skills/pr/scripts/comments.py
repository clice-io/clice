"""Print a PR's review findings without the bot boilerplate.

Unresolved review threads (every page, every comment) and the findings that
bots post in review bodies instead of inline threads, each reduced to
author, location, severity and the comment text. Badges, AI prompts,
walkthroughs, tracking comments and reaction footers are dropped.

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

# The markup bots wrap findings in. Anything else between angle brackets is
# code (`std::vector<int>`, `x < 0 && y > 1`) and stays.
HTML_TAGS = re.compile(
    r"</?(?:a|b|blockquote|br|code|details|div|em|h[1-6]|hr|i|img|li|ol|p|pre|span|strong"
    r"|sub|summary|sup|table|tbody|td|th|thead|tr|ul)\b[^>\n]*>",
    re.I,
)

CODE = re.compile(r"```.*?```|`[^`\n]*`", re.S)

NOISE_LINES = re.compile(
    r"Useful\? React with|Codex Review$|automated review suggestions|Reviewed commit:|^-{3,}$"
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
          comments(first: 100) {
            pageInfo { hasNextPage endCursor }
            nodes { author { login } body url }
          }
        }
      }
    }
  }
}
"""

THREAD_COMMENTS_QUERY = """
query($id: ID!, $after: String) {
  node(id: $id) {
    ... on PullRequestReviewThread {
      comments(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes { author { login } body url }
      }
    }
  }
}
"""

REVIEWS_QUERY = """
query($owner: String!, $name: String!, $pr: Int!, $before: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $pr) {
      reviews(last: 50, before: $before) {
        pageInfo { hasPreviousPage startCursor }
        nodes { author { login } state body url commit { abbreviatedOid } }
      }
    }
  }
}
"""


def gh(*args, status_codes=()):
    """Run gh and parse its JSON output.

    `status_codes` are exit codes the command uses to report state rather
    than failure (`gh pr checks` documents 8 for pending checks). Any other
    non-zero exit that still printed JSON and nothing on stderr is such a
    status too; a real failure prints its message on stderr and no JSON.
    """
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    time.sleep(1)
    if result.returncode == 0 or result.returncode in status_codes:
        return json.loads(result.stdout)
    if not result.stderr.strip():
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError:
            pass
    sys.exit(result.stderr.strip() or f"gh {' '.join(args)} failed")


def graphql(query, **variables):
    args = ["api", "graphql", "-f", f"query={query}"]
    for key, value in variables.items():
        if value is not None:
            args += ["-F" if isinstance(value, int) else "-f", f"{key}={value}"]
    return gh(*args)["data"]


def login(node):
    """The author's login; GitHub returns a null author for deleted accounts."""
    author = node.get("author")
    return author["login"] if author else "ghost"


def repo():
    info = gh("repo", "view", "--json", "owner,name")
    return info["owner"]["login"], info["name"]


def current_pr():
    return gh("pr", "view", "--json", "number")["number"]


def fetch_threads(owner, name, pr):
    threads, after, title = [], None, ""
    while True:
        page = graphql(THREADS_QUERY, owner=owner, name=name, pr=pr, after=after)[
            "repository"
        ]["pullRequest"]
        title = page["title"]
        threads += page["reviewThreads"]["nodes"]
        info = page["reviewThreads"]["pageInfo"]
        if not info["hasNextPage"]:
            break
        after = info["endCursor"]
    for thread in threads:
        comments = thread["comments"]
        while comments["pageInfo"]["hasNextPage"]:
            page = graphql(
                THREAD_COMMENTS_QUERY,
                id=thread["id"],
                after=comments["pageInfo"]["endCursor"],
            )
            comments["nodes"] += page["node"]["comments"]["nodes"]
            comments["pageInfo"] = page["node"]["comments"]["pageInfo"]
    return title, threads


def fetch_reviews(owner, name, pr):
    reviews, before = [], None
    while True:
        page = graphql(REVIEWS_QUERY, owner=owner, name=name, pr=pr, before=before)[
            "repository"
        ]["pullRequest"]["reviews"]
        reviews = page["nodes"] + reviews
        if not page["pageInfo"]["hasPreviousPage"]:
            return reviews
        before = page["pageInfo"]["startCursor"]


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


def strip_markup(text):
    """Remove HTML markup outside code spans and fenced blocks."""
    spans = []

    def stash(match):
        spans.append(match.group(0))
        return f"\x00{len(spans) - 1}\x00"

    text = CODE.sub(stash, text)
    text = HTML_TAGS.sub("", text)
    return re.sub(r"\x00(\d+)\x00", lambda m: spans[int(m.group(1))], text)


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
    text = strip_markup(text)
    text = re.sub(r"^(> ?)+", "", text, flags=re.M)
    text = re.sub(
        r"^\[!(?:CAUTION|NOTE|WARNING|TIP|IMPORTANT)\]\s*$", "", text, flags=re.M
    )
    text = re.sub(r"^\*\*[ \t]+", "**", text, flags=re.M)
    lines = [
        line.rstrip()
        for line in text.splitlines()
        if not NOISE_LINES.search(line.strip())
    ]
    text = "\n".join(lines).strip()
    return severity, re.sub(r"\n{3,}", "\n\n", text)


def is_finding(review, cleaned):
    """A bot review body counts when it carries a located finding rather than
    a per-commit "reviewed" notice or an all-clear; people's review bodies are
    not findings."""
    if login(review) not in BOTS:
        return False
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
            f"\n[{index}] {thread['id']}  {location}  {login(first)}  {flags}".rstrip()
        )
        print(indent(truncate(text, args.max_chars)))
        for reply in replies:
            _, reply_text = clean(reply["body"])
            if reply_text:
                print(
                    indent(
                        f"↳ {login(reply)}: {truncate(reply_text, args.max_chars // 2)}"
                    )
                )

    findings = []
    for review in reviews:
        severity, text = clean(review["body"])
        if is_finding(review, text):
            findings.append((review, severity, text))
    if findings:
        print("\nfindings in review bodies (no inline thread):")
        for review, severity, text in findings:
            commit = review["commit"]["abbreviatedOid"] if review["commit"] else "?"
            label = f"{login(review)} @ {commit} {severity}".rstrip()
            print(f"\n- {label}  {review['url']}")
            print(indent(truncate(text, args.max_chars)))


if __name__ == "__main__":
    main()
