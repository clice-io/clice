"""Tests for the snapshot tooling itself: annotation parser, snapshot
format and flows, URI validator. No server involved."""

import pytest
from lsprotocol.types import SemanticTokensLegend

from tests.integration.features.test_snapshots import decode_semantic_tokens
from tests.tools.annotation import parse_annotations
from tests.tools.snapshot import (
    Snapshot,
    SnapshotContext,
    fixture_frontmatter,
    format_snap,
    normalize_file_uri,
    parse_snap,
    yaml_str,
)


def test_annotation_passthrough():
    src = parse_annotations("int x = 1;\n")
    assert src.content == "int x = 1;\n"
    assert not src.offsets and not src.ranges and not src.nameless_offsets


def test_annotation_points_ranges():
    src = parse_annotations("int §(a)x = §1;\n§(r)⟦int §⟦y⟧;⟧\n")
    assert src.content == "int x = 1;\nint y;\n"
    assert src.offsets == {"a": 4}
    assert src.nameless_offsets == [8]
    assert src.ranges == {"r": (11, 17), "": (15, 16)}


def test_annotation_byte_offsets():
    # Offsets count UTF-8 bytes, matching the C++ side.
    src = parse_annotations("/*中*/§(p)x")
    assert src.content == "/*中*/x"
    assert src.offsets == {"p": 7}


def test_annotation_nameless_parens():
    src = parse_annotations("f§()(1)")
    assert src.content == "f(1)"
    assert src.nameless_offsets == [1]


@pytest.mark.parametrize(
    "text",
    [
        "§(unterminated",
        "§(not an identifier)",
        "§(café)",
        "§(dup)x §(dup)y",
        "no open⟧",
        "§(d)⟦x⟧ §(d)⟦y⟧",
        "bare ⟦",
        "§⟦unclosed",
    ],
)
def test_annotation_rejects_malformed(text):
    with pytest.raises(ValueError):
        parse_annotations(text)


def test_snap_format_round_trip():
    text = format_snap("s.py", "f.cpp", "body\n", "2026-01-01")
    assert parse_snap(text) == Snapshot("2026-01-01", "body\n")
    assert parse_snap("no frontmatter") is None


def test_snapshot_check_flows(tmp_path):
    ctx = SnapshotContext(tmp_path, update=False)
    ctx.check("a.cpp", "one\n")  # first run creates
    created_at = parse_snap((tmp_path / "a.cpp.snap.yml").read_text()).created_at
    ctx.check("a.cpp", "one\n")  # match passes

    with pytest.raises(AssertionError, match="snapshot mismatch"):
        ctx.check("a.cpp", "two\n")
    assert (tmp_path / "a.cpp.snap.yml.new").exists()

    SnapshotContext(tmp_path, update=True).check("a.cpp", "two\n")
    updated = parse_snap((tmp_path / "a.cpp.snap.yml").read_text())
    assert updated == Snapshot(created_at, "two\n")
    assert not (tmp_path / "a.cpp.snap.yml.new").exists()
    ctx.check("a.cpp", "two\n")


def test_normalize_file_uri(tmp_path):
    ws = tmp_path / "ws"
    ws.mkdir()
    inside = ws / "a b.h"
    inside.touch()
    outside = tmp_path / "outside.h"
    outside.touch()

    assert normalize_file_uri(inside.as_uri(), ws) == "${WS}/a b.h"
    assert "%20" in inside.as_uri()  # the positive case exercises decoding

    for bad in [
        str(inside),  # raw path, no scheme
        inside.as_uri().replace("%20", " "),  # missing percent-encoding
        "file:///tmp/%GG.h",  # malformed percent triplet
        "https://example.com/a.h",  # wrong scheme
        f"file://host{inside.as_posix()}",  # unexpected authority
        inside.as_uri() + "?query",
        inside.as_uri() + "#fragment",
        "file:relative.h",  # not absolute
        (ws / "missing.h").as_uri(),  # target does not exist
        outside.as_uri(),  # escapes the workspace
    ]:
        with pytest.raises(AssertionError):
            normalize_file_uri(bad, ws)


def test_yaml_str_escapes():
    assert yaml_str('a"b\\c\n\t\x01') == '"a\\"b\\\\c\\n\\t\\x01"'


def test_fixture_frontmatter():
    header = "/// # Title\n///\n/// - status: unsupported\nint x;\n"
    assert fixture_frontmatter(header, "status") == "unsupported"
    assert fixture_frontmatter(header, "missing") == ""
    assert fixture_frontmatter("int x;\n", "status") == ""


def test_semantic_token_decoding():
    legend = SemanticTokensLegend(
        token_types=["Type", "Function"], token_modifiers=["Definition", "Readonly"]
    )
    data = [0, 4, 3, 0, 1, 1, 2, 4, 1, 3]
    lines = ["abc defg", "xxfuncy"]
    assert decode_semantic_tokens(data, lines, legend) == [
        '- { loc: "0:4", text: "def", kind: Type, modifiers: [Definition] }',
        '- { loc: "1:2", text: "func", kind: Function, modifiers: [Definition, Readonly] }',
    ]
