import argparse
import json
import re
from pathlib import Path

FORBIDDEN = {"good first issue", "help wanted", "needs-triage"}
CONFIDENCE = {"high", "medium", "low"}


def extract_array(text):
    decoder = json.JSONDecoder()
    for match in re.finditer(r"\[", text):
        try:
            data, end = decoder.raw_decode(text, match.start())
        except ValueError:
            continue
        if isinstance(data, list):
            return data
    return None


def check(verdict, taxonomy, existing):
    labels = set(verdict.get("labels") or [])
    labels.discard("needs-triage")
    problems = []
    unknown = labels - taxonomy
    if unknown:
        problems.append(f"unknown labels {sorted(unknown)}")
    kinds = [l for l in labels if l.startswith("kind:")]
    if len(kinds) != 1:
        problems.append(f"{len(kinds)} kind labels")
    banned = (labels - existing) & FORBIDDEN
    if banned:
        problems.append(f"forbidden additions {sorted(banned)}")
    if "os:wsl" in labels and labels & {"os:linux", "os:macos", "os:windows"}:
        problems.append("os:wsl combined with a native os label")
    if verdict.get("confidence") not in CONFIDENCE:
        problems.append(f"bad confidence {verdict.get('confidence')!r}")
    return labels, problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "verdicts",
        nargs="+",
        help="codex output files (JSON array, possibly wrapped in prose)",
    )
    parser.add_argument("--labels", default=".github/labels.yml")
    parser.add_argument("--snapshot", default="/tmp/clice-triage")
    args = parser.parse_args()

    taxonomy = set(re.findall(r'^- name: "(.+)"$', Path(args.labels).read_text(), re.M))
    if not taxonomy:
        raise SystemExit(f"no labels parsed from {args.labels}")

    snapshot = Path(args.snapshot)
    existing = json.loads((snapshot / "existing-labels.json").read_text())
    expected = {int(p.stem) for p in snapshot.glob("issues/chunk-*/*.md")}

    verdicts = {}
    failures = []
    for file in args.verdicts:
        data = extract_array(Path(file).read_text())
        if data is None:
            failures.append((file, "no JSON array found"))
            continue
        for verdict in data:
            try:
                verdicts[int(verdict["issue"])] = verdict
            except (TypeError, KeyError, ValueError):
                failures.append((file, f"malformed entry: {str(verdict)[:80]}"))

    valid = []
    for num in sorted(expected):
        if num not in verdicts:
            failures.append((num, "no verdict returned"))
            continue
        verdict = verdicts[num]
        try:
            existing_set = set(existing.get(str(num), []))
            labels, problems = check(verdict, taxonomy, existing_set)
        except Exception as error:
            failures.append((num, f"malformed verdict: {error}"))
            continue
        if problems:
            failures.append((num, "; ".join(problems)))
            continue
        verdict["labels"] = sorted(labels)
        verdict["add"] = sorted(labels - existing_set)
        verdict["suggest_remove"] = sorted(existing_set - labels - {"needs-triage"})
        valid.append(verdict)

    hallucinated = sorted(set(verdicts) - expected)
    (snapshot / "validated.json").write_text(json.dumps(valid, indent=1))

    for verdict in valid:
        print(
            f"#{verdict['issue']} [{verdict['confidence']}] "
            f"+{verdict['add']} — {verdict.get('rationale', '')}"
        )
        if verdict.get("suggest_remove"):
            print(f"    suggest removing (manual) → {verdict['suggest_remove']}")
        if verdict.get("better_title"):
            print(f"    title → {verdict['better_title']}")
        if verdict.get("ask_reporter"):
            print(f"    ask → {verdict['ask_reporter']}")
        if verdict.get("taxonomy_gap"):
            print(f"    gap → {verdict['taxonomy_gap']}")
    print(f"\nvalid: {len(valid)}/{len(expected)} → {snapshot / 'validated.json'}")
    for source, why in failures:
        print(f"FAILED {source}: {why}")
    if hallucinated:
        print(f"verdicts for issues not in snapshot (dropped): {hallucinated}")


main()
