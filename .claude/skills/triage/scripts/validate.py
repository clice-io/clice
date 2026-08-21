import argparse
import json
import re
from pathlib import Path

FORBIDDEN = {"good first issue", "help wanted"}
CONFIDENCE = {"high", "medium", "low"}


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
    for file in args.verdicts:
        match = re.search(r"\[.*\]", Path(file).read_text(), re.S)
        if not match:
            raise SystemExit(f"no JSON array found in {file}")
        for verdict in json.loads(match.group(0)):
            verdicts[verdict["issue"]] = verdict

    valid = []
    failures = []
    for num in sorted(expected):
        if num not in verdicts:
            failures.append((num, "no verdict returned"))
            continue
        verdict = verdicts[num]
        labels = set(verdict["labels"])
        problems = []
        unknown = labels - taxonomy
        if unknown:
            problems.append(f"unknown labels {sorted(unknown)}")
        kinds = [l for l in labels if l.startswith("kind:")]
        if len(kinds) != 1:
            problems.append(f"{len(kinds)} kind labels")
        banned = labels & FORBIDDEN
        if banned:
            problems.append(f"forbidden labels {sorted(banned)}")
        if verdict.get("confidence") not in CONFIDENCE:
            problems.append(f"bad confidence {verdict.get('confidence')!r}")
        if problems:
            failures.append((num, "; ".join(problems)))
            continue
        verdict["add"] = sorted(labels - set(existing.get(str(num), [])))
        valid.append(verdict)

    hallucinated = sorted(set(verdicts) - expected)
    (snapshot / "validated.json").write_text(json.dumps(valid, indent=1))

    for verdict in valid:
        line = f"#{verdict['issue']} [{verdict['confidence']}] +{verdict['add']} — {verdict['rationale']}"
        print(line)
        if verdict.get("better_title"):
            print(f"    title → {verdict['better_title']}")
        if verdict.get("ask_reporter"):
            print(f"    ask → {verdict['ask_reporter']}")
        if verdict.get("taxonomy_gap"):
            print(f"    gap → {verdict['taxonomy_gap']}")
    print(f"\nvalid: {len(valid)}/{len(expected)} → {snapshot / 'validated.json'}")
    for num, why in failures:
        print(f"FAILED #{num}: {why}")
    if hallucinated:
        print(f"verdicts for issues not in snapshot (dropped): {hallucinated}")


main()
