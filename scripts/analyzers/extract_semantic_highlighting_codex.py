#!/usr/bin/env python3
"""Use Codex to analyze clangd semantic-highlighting logic from a GitHub blob URL.

Example:
    python3 scripts/analyzers/extract_semantic_highlighting_codex.py \
        https://github.com/llvm/llvm-project/blob/d8ba56ce3f98871ae4e5782c4af2df4c98bebde7/clang-tools-extra/clangd/SemanticHighlighting.cpp \
        --output semantic-highlighting.md
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Literal
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import ProxyHandler, Request, build_opener, getproxies


PROMPT = """You are analyzing one C++ source file from clangd.

Task:
- Extract the cases in which semantic highlighting is applied.
- Produce an exhaustive segmentation that covers every source line exactly once.

Kinds:
- nop: this line does not materially participate in deciding or applying a highlight.
- condition: this line or contiguous range establishes a boolean/branching condition that gates a later highlight resolution.
- resolution: this line or contiguous range selects or applies a concrete semantic-highlighting outcome, such as a HighlightingKind, modifier, token emission, or an equivalent concrete result.

Output rules:
- Return JSON only.
- Segments must be in ascending order and non-overlapping.
- Every line from 1 through the last line must be covered exactly once.
- Use the smallest practical contiguous ranges.
- It is fine to compress long nop runs into ranges; the caller may expand them later.
- For nop segments, use an empty summary string.
- For condition segments, write one short sentence in plain English.
- For resolution segments, write one short sentence describing the concrete outcome.
- For resolution segments, populate depends_on with the exact condition line ranges that directly gate this outcome when present.
- Use the provided line numbers exactly; never invent lines.
- Do not use any external tools or local files; analyze only the numbered source text provided in the prompt.
"""


SCHEMA = {
    "type": "object",
    "properties": {
        "segments": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "start_line": {"type": "integer", "minimum": 1},
                    "end_line": {"type": "integer", "minimum": 1},
                    "kind": {
                        "type": "string",
                        "enum": ["nop", "condition", "resolution"],
                    },
                    "summary": {"type": "string"},
                    "depends_on": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "start_line": {"type": "integer", "minimum": 1},
                                "end_line": {"type": "integer", "minimum": 1},
                            },
                            "required": ["start_line", "end_line"],
                            "additionalProperties": False,
                        },
                    },
                },
                "required": [
                    "start_line",
                    "end_line",
                    "kind",
                    "summary",
                    "depends_on",
                ],
                "additionalProperties": False,
            },
        }
    },
    "required": ["segments"],
    "additionalProperties": False,
}


SegmentKind = Literal["nop", "condition", "resolution"]

PROXY_ENV_KEYS = ("http_proxy", "https_proxy", "all_proxy", "no_proxy")


@dataclass(frozen=True)
class GitHubBlobRef:
    owner: str
    repo: str
    rev: str
    path: str
    blob_url: str
    raw_url: str

    @property
    def title(self) -> str:
        return Path(self.path).stem


@dataclass(frozen=True)
class LineRange:
    start_line: int
    end_line: int


@dataclass(frozen=True)
class Segment:
    start_line: int
    end_line: int
    kind: SegmentKind
    summary: str
    depends_on: tuple[LineRange, ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Ask Codex to analyze a GitHub-hosted semantic-highlighting file.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "url",
        help="GitHub blob URL pinned to a specific revision.",
    )
    parser.add_argument(
        "--model",
        default=None,
        help="Codex model to use via `codex exec`.",
    )
    parser.add_argument(
        "--reasoning-effort",
        default=None,
        choices=["low", "medium", "high", "xhigh"],
        help="Reasoning effort override passed to Codex CLI.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=30,
        help="HTTP timeout in seconds for fetching the source file.",
    )
    parser.add_argument(
        "--codex-bin",
        default="codex",
        help="Codex CLI executable to invoke.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write the markdown output to this path instead of stdout.",
    )
    return parser.parse_args()


def parse_github_blob_url(url: str) -> GitHubBlobRef:
    parsed = urlparse(url)
    if parsed.scheme not in {"http", "https"} or parsed.netloc != "github.com":
        raise ValueError("expected a GitHub https://github.com/.../blob/... URL")

    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) < 5 or parts[2] != "blob":
        raise ValueError("expected a GitHub blob URL with /owner/repo/blob/rev/path")

    owner, repo = parts[0], parts[1]
    rev = parts[3]
    path = "/".join(parts[4:])
    if not path:
        raise ValueError("missing file path in GitHub blob URL")

    raw_url = f"https://raw.githubusercontent.com/{owner}/{repo}/{rev}/{path}"
    normalized_blob_url = f"https://github.com/{owner}/{repo}/blob/{rev}/{path}"
    return GitHubBlobRef(
        owner=owner,
        repo=repo,
        rev=rev,
        path=path,
        blob_url=normalized_blob_url,
        raw_url=raw_url,
    )


def fetch_text(url: str, timeout: int) -> str:
    request = Request(url, headers={"User-Agent": "semantic-highlighting-codex/1.0"})
    opener = build_opener(ProxyHandler(getproxies()))
    try:
        with opener.open(request, timeout=timeout) as response:
            return response.read().decode("utf-8").replace("\r\n", "\n")
    except HTTPError as exc:
        raise RuntimeError(f"failed to fetch {url}: HTTP {exc.code}") from exc
    except URLError as exc:
        raise RuntimeError(f"failed to fetch {url}: {exc.reason}") from exc


def number_source(text: str) -> tuple[str, int]:
    lines = text.splitlines()
    width = len(str(max(len(lines), 1)))
    numbered = "\n".join(
        f"{idx:>{width}} | {line}" for idx, line in enumerate(lines, 1)
    )
    return numbered, len(lines)


def build_codex_prompt(
    blob: GitHubBlobRef,
    source_text: str,
    line_count: int,
) -> str:
    numbered_source, _ = number_source(source_text)
    return (
        f"{PROMPT}\n\n"
        f"GitHub blob URL: {blob.blob_url}\n"
        f"LLVM revision: {blob.rev}\n"
        f"File path: {blob.path}\n"
        f"File title: {blob.title}\n"
        f"Total lines: {line_count}\n\n"
        "Analyze only the following numbered source file:\n"
        f"{numbered_source}\n"
    )


def analyze_with_codex_cli(
    codex_bin: str,
    blob: GitHubBlobRef,
    source_text: str,
    line_count: int,
    model: str | None,
    reasoning_effort: str | None,
) -> list[Segment]:
    if shutil.which(codex_bin) is None:
        raise RuntimeError(f"Codex CLI executable `{codex_bin}` was not found in PATH.")

    prompt = build_codex_prompt(
        blob=blob, source_text=source_text, line_count=line_count
    )
    child_env = build_codex_env()

    with tempfile.TemporaryDirectory(prefix="semantic-highlighting-codex-") as temp_dir:
        temp_path = Path(temp_dir)
        schema_path = temp_path / "schema.json"
        output_path = temp_path / "last-message.json"
        schema_path.write_text(json.dumps(SCHEMA, indent=2), encoding="utf-8")

        command = [
            codex_bin,
            "exec",
            "--skip-git-repo-check",
            "--ephemeral",
            "--color",
            "never",
            "--sandbox",
            "read-only",
            *([] if model is None else ["--model", model]),
            *(
                []
                if reasoning_effort is None
                else ["--config", f"model_reasoning_effort={json.dumps(reasoning_effort)}"]
            ),
            "--output-schema",
            str(schema_path),
            "--output-last-message",
            str(output_path),
            "-",
        ]

        try:
            completed = subprocess.run(
                command,
                input=prompt,
                text=True,
                capture_output=True,
                check=True,
                env=child_env,
            )
        except FileNotFoundError as exc:
            raise RuntimeError(
                f"failed to execute `{codex_bin}`: command not found"
            ) from exc
        except subprocess.CalledProcessError as exc:
            detail = exc.stderr.strip() or exc.stdout.strip() or str(exc)
            raise RuntimeError(f"Codex CLI failed: {detail}") from exc

        if not output_path.exists():
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise RuntimeError(
                "Codex CLI did not produce an output message."
                + (f" Details: {detail}" if detail else "")
            )

        output_text = output_path.read_text(encoding="utf-8").strip()
        if not output_text:
            raise RuntimeError("Codex CLI returned an empty final message.")

    try:
        payload = json.loads(output_text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Codex returned invalid JSON:\n{output_text}") from exc

    return normalize_segments(payload.get("segments", []), line_count)


def build_codex_env() -> dict[str, str]:
    env = os.environ.copy()
    for key in PROXY_ENV_KEYS:
        value = env.get(key)
        upper_key = key.upper()
        if value and upper_key not in env:
            env[upper_key] = value
    return env


def normalize_segments(raw_segments: list[dict], line_count: int) -> list[Segment]:
    normalized: list[Segment] = []
    for item in raw_segments:
        kind = item["kind"]
        if kind not in {"nop", "condition", "resolution"}:
            raise ValueError(f"unknown segment kind: {kind}")
        start_line = int(item["start_line"])
        end_line = int(item["end_line"])
        if start_line < 1 or end_line < start_line or end_line > line_count:
            raise ValueError(
                f"invalid segment range {start_line}-{end_line} for file with {line_count} lines"
            )
        depends_on = tuple(
            LineRange(start_line=int(dep["start_line"]), end_line=int(dep["end_line"]))
            for dep in item.get("depends_on", [])
        )
        normalized.append(
            Segment(
                start_line=start_line,
                end_line=end_line,
                kind=kind,
                summary=item.get("summary", "").strip(),
                depends_on=depends_on,
            )
        )

    normalized.sort(key=lambda segment: (segment.start_line, segment.end_line))

    stitched: list[Segment] = []
    next_line = 1
    for segment in normalized:
        if segment.start_line < next_line:
            raise ValueError(
                f"overlapping segments around line {segment.start_line}: model output is invalid"
            )
        while next_line < segment.start_line:
            stitched.append(
                Segment(
                    start_line=next_line,
                    end_line=next_line,
                    kind="nop",
                    summary="",
                    depends_on=(),
                )
            )
            next_line += 1
        stitched.extend(expand_nop_segment(segment))
        next_line = segment.end_line + 1

    while next_line <= line_count:
        stitched.append(
            Segment(
                start_line=next_line,
                end_line=next_line,
                kind="nop",
                summary="",
                depends_on=(),
            )
        )
        next_line += 1

    return stitched


def expand_nop_segment(segment: Segment) -> list[Segment]:
    if segment.kind != "nop" or segment.start_line == segment.end_line:
        return [segment]
    return [
        Segment(
            start_line=line_no,
            end_line=line_no,
            kind="nop",
            summary="",
            depends_on=(),
        )
        for line_no in range(segment.start_line, segment.end_line + 1)
    ]


def blob_anchor_url(blob: GitHubBlobRef, line_range: LineRange) -> str:
    if line_range.start_line == line_range.end_line:
        return f"{blob.blob_url}#L{line_range.start_line}"
    return f"{blob.blob_url}#L{line_range.start_line}-L{line_range.end_line}"


def format_line_range(start_line: int, end_line: int) -> str:
    if start_line == end_line:
        return f"Line {start_line}"
    return f"Line {start_line}-{end_line}"


def format_loc_reference(blob: GitHubBlobRef, line_range: LineRange) -> str:
    label = (
        f"LoC {line_range.start_line}"
        if line_range.start_line == line_range.end_line
        else f"LoC {line_range.start_line}-{line_range.end_line}"
    )
    return f"[{label}]({blob_anchor_url(blob, line_range)})"


def render_segment_summary(blob: GitHubBlobRef, segment: Segment) -> str:
    summary = segment.summary.strip()
    if segment.kind == "nop" or not summary:
        return ""
    if segment.kind == "resolution" and segment.depends_on:
        refs = [format_loc_reference(blob, dep) for dep in segment.depends_on]
        if len(refs) == 1:
            prefix = f"By condition at {refs[0]}, "
        else:
            prefix = f"By conditions at {', '.join(refs)}, "
        return prefix + decapitalize_summary(summary)
    return summary


def decapitalize_summary(summary: str) -> str:
    if not summary:
        return summary
    if len(summary) >= 2 and summary[0].isupper() and summary[1].islower():
        return summary[:1].lower() + summary[1:]
    return summary


def render_markdown(blob: GitHubBlobRef, segments: list[Segment]) -> str:
    lines = [f"- LLVMRevHash: `{blob.rev}`", f"# {blob.title}", ""]
    for segment in segments:
        lines.append(
            f"## {format_line_range(segment.start_line, segment.end_line)} (kind: {segment.kind})"
        )
        summary = render_segment_summary(blob, segment)
        if summary:
            lines.append(summary)
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    args = parse_args()

    try:
        blob = parse_github_blob_url(args.url)
        source_text = fetch_text(blob.raw_url, timeout=args.timeout)
        _, line_count = number_source(source_text)
        segments = analyze_with_codex_cli(
            codex_bin=args.codex_bin,
            blob=blob,
            source_text=source_text,
            line_count=line_count,
            model=args.model,
            reasoning_effort=args.reasoning_effort,
        )
        markdown = render_markdown(blob, segments)
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    if args.output:
        args.output.write_text(markdown, encoding="utf-8")
    else:
        sys.stdout.write(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
