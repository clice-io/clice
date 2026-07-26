"""Snapshot assertions for integration tests, mirroring zest's semantics.

File format, layout and update workflow follow the C++ side
(kota::zest snapshot support) so both layers share one muscle memory:

    ---
    source: test_snapshots.py
    created_at: 2026-07-26
    input_file: fold_kinds/block_folding.cpp
    ---
    <body>

A missing snapshot is created and passes; a mismatch writes a sibling
`.snap.yml.new` and fails with a diff; `--update-snapshots` rewrites the
stored snapshot preserving its created_at.
"""

import difflib
import re
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from urllib.parse import urlsplit

import pygls.uris

WORKSPACE_PLACEHOLDER = "${WS}"

# Bytes RFC 3986 does not allow to appear raw in a URI (spaces, control
# characters, non-ASCII, ...). Their presence means the server skipped
# percent-encoding; urlsplit/unquote would silently tolerate them.
URI_ILLEGAL_CHAR = re.compile(r"[^A-Za-z0-9\-._~:/?#\[\]@!$&'()*+,;=%]")
URI_BAD_PERCENT = re.compile(r"%(?![0-9A-Fa-f]{2})")


def normalize_file_uri(uri: str, workspace: Path) -> str:
    """Validate a wire URI and rewrite it to a platform-independent form.

    This is deliberately an invariant check, not a best-effort cleanup: any
    server response field holding a file reference must be a well-formed
    `file://` URI whose decoded path exists inside the test workspace.
    A raw filesystem path fails here on every platform, instead of only
    breaking real clients on Windows.
    """
    if bad := URI_ILLEGAL_CHAR.search(uri):
        raise AssertionError(
            f"raw {bad.group()!r} in URI (missing percent-encoding): {uri!r}"
        )
    if URI_BAD_PERCENT.search(uri):
        raise AssertionError(f"malformed percent-encoding in URI: {uri!r}")

    split = urlsplit(uri)
    if split.scheme != "file":
        raise AssertionError(f"not a file:// URI (scheme={split.scheme!r}): {uri!r}")
    if split.netloc:
        raise AssertionError(f"unexpected authority in file URI: {uri!r}")
    if split.query or split.fragment:
        raise AssertionError(f"unexpected query/fragment in file URI: {uri!r}")

    # pygls owns the percent-decoding and Windows drive-letter handling;
    # the checks above and below are ours.
    path = pygls.uris.to_fs_path(uri)
    if path is None:
        raise AssertionError(f"URI does not decode to a filesystem path: {uri!r}")

    p = Path(path)
    if not p.is_absolute():
        raise AssertionError(f"URI does not decode to an absolute path: {uri!r}")
    if not p.exists():
        raise AssertionError(f"URI target does not exist on disk: {uri!r} -> {p}")

    resolved = p.resolve()
    ws = workspace.resolve()
    if not resolved.is_relative_to(ws):
        raise AssertionError(f"URI target escapes the workspace {ws}: {uri!r}")
    return f"{WORKSPACE_PLACEHOLDER}/{resolved.relative_to(ws).as_posix()}"


def yaml_str(s: str) -> str:
    """Quote a string exactly like tests/unit/test/tester.h yaml_str, so
    snapshot bodies read identically across the two layers."""
    out = ['"']
    for c in s:
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            out.append("\\r")
        elif c == "\t":
            out.append("\\t")
        elif ord(c) < 0x20:
            out.append(f"\\x{ord(c):02x}")
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def fixture_frontmatter(content: str, key: str) -> str:
    """Value of `- key: value` in a fixture's leading `///` doc header, or
    "". Mirrors tests/unit/test/fixture.h; feature_docs.py is the authority
    on the full grammar."""
    first = content.split("\n", 1)[0].strip()
    if not first.startswith("///"):
        return ""
    first = first[3:].removeprefix(" ")
    if not first.startswith("# "):
        return ""
    for line in content.split("\n"):
        line = line.strip()
        if not line.startswith("///"):
            break
        line = line[3:].strip()
        if not line.startswith("- ") or ":" not in line:
            continue
        k, v = line[2:].split(":", 1)
        if k.strip() == key:
            return v.strip()
    return ""


@dataclass
class Snapshot:
    created_at: str
    body: str


def parse_snap(text: str) -> Snapshot | None:
    lines = text.replace("\r\n", "\n").split("\n")
    if not lines or lines[0] != "---":
        return None
    created_at = ""
    for i, line in enumerate(lines[1:], start=1):
        if line == "---":
            return Snapshot(created_at, "\n".join(lines[i + 1 :]))
        if line.startswith("created_at:"):
            created_at = line.split(":", 1)[1].strip()
    return None


def format_snap(source: str, input_file: str, body: str, created_at: str = "") -> str:
    if not created_at:
        created_at = date.today().isoformat()
    if body and not body.endswith("\n"):
        body += "\n"
    return (
        f"---\n"
        f"source: {source}\n"
        f"created_at: {created_at}\n"
        f"input_file: {input_file}\n"
        f"---\n"
        f"{body}"
    )


@dataclass
class SnapshotContext:
    """One feature's snapshot directory plus the session-wide update flag."""

    directory: Path
    update: bool
    source: str = "test_snapshots.py"

    def check(self, input_file: str, body: str) -> None:
        body = body.replace("\r\n", "\n")
        if body and not body.endswith("\n"):
            body += "\n"

        snap_path = self.directory / f"{input_file}.snap.yml"
        new_path = snap_path.with_name(snap_path.name + ".new")

        existing = parse_snap(snap_path.read_text()) if snap_path.exists() else None
        if existing is None:
            snap_path.parent.mkdir(parents=True, exist_ok=True)
            snap_path.write_text(format_snap(self.source, input_file, body))
            print(f"[snapshot] created {snap_path}")
            return

        if existing.body == body:
            new_path.unlink(missing_ok=True)
            return

        if self.update:
            snap_path.write_text(
                format_snap(self.source, input_file, body, existing.created_at)
            )
            new_path.unlink(missing_ok=True)
            print(f"[snapshot] updated {snap_path}")
            return

        new_path.write_text(format_snap(self.source, input_file, body))
        diff = "\n".join(
            difflib.unified_diff(
                existing.body.splitlines(),
                body.splitlines(),
                fromfile=str(snap_path),
                tofile="new result",
                lineterm="",
            )
        )
        raise AssertionError(
            f"snapshot mismatch: {snap_path}\n"
            f"new result written to {new_path}\n"
            f"{diff}\n"
            f"run with --update-snapshots to accept"
        )
