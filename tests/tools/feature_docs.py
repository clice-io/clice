"""Generate feature-doc checklist sections from snapshot fixtures.

Each fixture .cpp under tests/data/<feature>/ may carry a spec header: a
block of `///` lines at the very top whose keys describe one checklist
capability. This tool renders those headers into the GENERATED regions of
docs/en/features/*.md, so the fixtures are the single source of truth and
the doc checklist is derived from them.

Fixture spec header:

    /// @section Fold Kinds
    /// @title Block folding — functions, classes, ...
    /// @status supported
    /// @issues clangd#1455, vscode#70794
    /// @order 1
    ///
    /// Optional markdown description after a bare `///` separator.

@section, @title and @status are required; @status is `supported`,
`partial` or `unsupported`. `partial` items render unchecked with a
*(partial)* marker but are still compiled and snapshotted, so the snapshot
records the current partial behavior; only `unsupported` fixtures are
skipped by the snapshot glob. Everything after the header (trimmed of
blank lines) is the example code, rendered verbatim into the doc. Fixtures
without any `@` key are supplementary edge-case tests and are excluded
from docs.

Usage:
    python tests/tools/feature_docs.py update   # rewrite generated regions
    python tests/tools/feature_docs.py check     # fail if regions are stale
"""

import argparse
import difflib
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# feature -> doc path (relative to repo root). Extend as more features
# adopt fixture-generated docs.
FEATURES = {
    "folding_range": "docs/en/features/folding-ranges.md",
}

ISSUE_TRACKERS = {
    "clangd": "https://github.com/clangd/clangd/issues/",
    "vscode": "https://github.com/microsoft/vscode/issues/",
}

REQUIRED_KEYS = ("section", "title", "status")
KNOWN_KEYS = ("section", "title", "status", "issues", "order")
VALID_STATUS = ("supported", "partial", "unsupported")

BEGIN_RE = re.compile(r"<!-- BEGIN GENERATED ITEMS: (.+?) -->")
END_MARKER = "<!-- END GENERATED ITEMS -->"
ISSUE_RE = re.compile(r"^([a-z]+)#(\d+)$")


@dataclass
class Fixture:
    path: Path
    section: str
    title: str
    status: str
    issues: list[str]
    order: int | None
    description: str
    example: str


def strip_comment(line: str) -> str:
    """Return the text of a `///` comment line, minus prefix and one space."""
    text = line.lstrip()[3:]
    if text.startswith(" "):
        text = text[1:]
    return text


def trim_blank(lines: list[str]) -> list[str]:
    while lines and lines[0].strip() == "":
        lines.pop(0)
    while lines and lines[-1].strip() == "":
        lines.pop()
    return lines


def parse_fixture(path: Path, problems: list[str]) -> Fixture | None:
    """Parse a fixture's spec header. Returns None for supplementary files."""
    lines = path.read_text(encoding="utf-8").splitlines()

    header: list[str] = []
    body_start = len(lines)
    for i, line in enumerate(lines):
        if line.lstrip().startswith("///"):
            header.append(line)
        else:
            body_start = i
            break

    keys: dict[str, str] = {}
    desc: list[str] = []
    in_desc = False
    for raw in header:
        content = strip_comment(raw)
        if not in_desc:
            if content.strip() == "":
                in_desc = True
                continue
            match = re.match(r"@(\w+)\s*(.*)", content)
            if match:
                key = match.group(1)
                if key in keys:
                    problems.append(f"{path}: duplicate @{key}")
                keys[key] = match.group(2).strip()
                continue
            in_desc = True
        desc.append(content)

    if not keys:
        # No spec keys at all: supplementary edge-case fixture, not a doc item.
        return None

    for key in REQUIRED_KEYS:
        if key not in keys:
            problems.append(f"{path}: missing required @{key}")
        elif not keys[key]:
            problems.append(f"{path}: empty @{key}")
    for key in keys:
        if key not in KNOWN_KEYS:
            problems.append(f"{path}: unknown key @{key}")
    status = keys.get("status", "")
    if "status" in keys and status not in VALID_STATUS:
        problems.append(
            f"{path}: invalid @status '{status}' (expected one of {', '.join(VALID_STATUS)})"
        )

    issues: list[str] = []
    for ref in (r.strip() for r in keys.get("issues", "").split(",")):
        if not ref:
            continue
        match = ISSUE_RE.match(ref)
        if not match or match.group(1) not in ISSUE_TRACKERS:
            problems.append(f"{path}: unknown issue reference '{ref}'")
            continue
        issues.append(ref)

    order = None
    if "order" in keys:
        try:
            order = int(keys["order"])
        except ValueError:
            problems.append(f"{path}: @order must be an integer, got '{keys['order']}'")

    example = "\n".join(trim_blank(lines[body_start:]))
    if not example.strip():
        problems.append(f"{path}: spec fixture has no example code")

    return Fixture(
        path=path,
        section=keys.get("section", ""),
        title=keys.get("title", ""),
        status=status,
        issues=issues,
        order=order,
        description="\n".join(trim_blank(desc)),
        example=example,
    )


def render_issue(ref: str) -> str:
    tracker, number = ref.split("#", 1)
    return f"[{ref}]({ISSUE_TRACKERS[tracker]}{number})"


def indent(text: str, prefix: str = "  ") -> list[str]:
    return [(prefix + line).rstrip() for line in text.split("\n")]


def render_item(fx: Fixture) -> str:
    box = "[x]" if fx.status == "supported" else "[ ]"
    line = f"- {box} {fx.title}"
    # Underscore emphasis matches prettier's markdown style, so `pixi run
    # format` leaves the generated regions untouched.
    if fx.status == "partial":
        line += " _(partial)_"
    if fx.issues:
        line += " (" + ", ".join(render_issue(ref) for ref in fx.issues) + ")"

    out = [line]
    if fx.description:
        out.append("")
        out.extend(indent(fx.description))
    if fx.example:
        out.append("")
        out.append("  ```cpp")
        out.extend(indent(fx.example))
        out.append("  ```")
    return "\n".join(out)


def collect_fixtures(feature: str, problems: list[str]) -> list[Fixture]:
    data_dir = REPO_ROOT / "tests" / "data" / feature
    fixtures: list[Fixture] = []
    titles: dict[str, Path] = {}
    for path in sorted(data_dir.glob("**/*.cpp")):
        fx = parse_fixture(path, problems)
        if fx is None:
            continue
        if fx.title in titles:
            problems.append(
                f"{path}: duplicate @title '{fx.title}' (also in {titles[fx.title]})"
            )
        else:
            titles[fx.title] = path
        fixtures.append(fx)
    fixtures.sort(
        key=lambda fx: (fx.order if fx.order is not None else 1 << 30, fx.path.name)
    )
    return fixtures


def render_region(section: str, fixtures: list[Fixture]) -> str:
    items = [render_item(fx) for fx in fixtures if fx.section == section]
    return "\n\n".join(items)


def rewrite_doc(
    doc_text: str,
    sections: dict[str, list[Fixture]],
    doc_path: Path,
    problems: list[str],
) -> str:
    lines = doc_text.split("\n")
    out: list[str] = []
    doc_sections: set[str] = set()

    idx = 0
    while idx < len(lines):
        line = lines[idx]
        match = BEGIN_RE.search(line)
        if not match:
            out.append(line)
            idx += 1
            continue

        section = match.group(1).strip()
        doc_sections.add(section)
        end = idx + 1
        while end < len(lines) and END_MARKER not in lines[end]:
            end += 1
        if end >= len(lines):
            problems.append(f"{doc_path}: region '{section}' has no closing marker")
            out.extend(lines[idx:])
            return "\n".join(out)

        matched = sections.get(section, [])
        if not matched:
            problems.append(f"{doc_path}: region '{section}' matches no fixtures")

        out.append(line)
        content = render_region(section, matched)
        out.append("")
        if content:
            out.append(content)
            out.append("")
        out.append(lines[end])
        idx = end + 1

    for section in sections:
        if section not in doc_sections:
            problems.append(
                f"{doc_path}: @section '{section}' has no matching marker region"
            )

    return "\n".join(out)


def process_feature(
    feature: str, doc_rel: str, problems: list[str]
) -> tuple[Path, str, str]:
    doc_path = REPO_ROOT / doc_rel
    fixtures = collect_fixtures(feature, problems)

    sections: dict[str, list[Fixture]] = {}
    for fx in fixtures:
        sections.setdefault(fx.section, []).append(fx)

    current = doc_path.read_text(encoding="utf-8")
    updated = rewrite_doc(current, sections, doc_path, problems)
    return doc_path, current, updated


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", choices=("update", "check"))
    args = parser.parse_args(argv)

    problems: list[str] = []
    results = [process_feature(f, d, problems) for f, d in FEATURES.items()]

    if problems:
        print("feature_docs: problems found:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    stale = False
    for doc_path, current, updated in results:
        if current == updated:
            continue
        stale = True
        if args.mode == "update":
            doc_path.write_text(updated, encoding="utf-8")
            print(f"updated {doc_path.relative_to(REPO_ROOT)}")
        else:
            diff = difflib.unified_diff(
                current.splitlines(keepends=True),
                updated.splitlines(keepends=True),
                fromfile=f"{doc_path.relative_to(REPO_ROOT)} (current)",
                tofile=f"{doc_path.relative_to(REPO_ROOT)} (generated)",
            )
            sys.stderr.writelines(diff)

    if args.mode == "check" and stale:
        print(
            "feature_docs: docs are stale; run 'feature_docs.py update'",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
