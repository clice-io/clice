"""Wire-level snapshot tests over the fixture corpora shared with the unit
snapshot glob; results are pinned under tests/snapshots/integration/."""

from pathlib import Path

import pytest
from lsprotocol.types import DocumentSymbol, InlayHintKind, Position, Range

from tests.tools.annotation import parse_annotations
from tests.tools.snapshot import (
    SnapshotContext,
    fixture_frontmatter,
    normalize_file_uri,
    yaml_str,
)

TESTS_DIR = Path(__file__).resolve().parents[2]
DATA_DIR = TESTS_DIR / "data"
SNAPSHOT_DIR = TESTS_DIR / "snapshots" / "integration"


def fmt_pos(pos: Position) -> str:
    return f"{pos.line}:{pos.character}"


def fmt_range(range_: Range) -> str:
    return f"{fmt_pos(range_.start)}-{fmt_pos(range_.end)}"


async def present_document_links(client, uri, source, workspace):
    links = await client.document_links(uri)
    for link in links or []:
        assert link.target is not None, "clice always resolves link targets"
    return [
        f'- {{ range: "{fmt_range(link.range)}", '
        f"target: {yaml_str(normalize_file_uri(link.target, workspace))} }}"
        for link in links or []
    ]


async def present_folding_ranges(client, uri, source, workspace):
    out = []
    for r in await client.folding_ranges(uri) or []:
        line = (
            f'- {{ range: "{r.start_line}:{r.start_character}'
            f'-{r.end_line}:{r.end_character}"'
        )
        if r.kind is not None:
            kind = r.kind.value if hasattr(r.kind, "value") else r.kind
            line += f", kind: {kind}"
        if r.collapsed_text:
            line += f", collapsed_text: {yaml_str(r.collapsed_text)}"
        out.append(line + " }")
    return out


async def present_document_symbols(client, uri, source, workspace):
    symbols = await client.document_symbols(uri)

    out = []

    def walk(symbol: DocumentSymbol, depth: int) -> None:
        line = (
            f"-{' ' * (1 + 2 * depth)}{{ name: {yaml_str(symbol.name)}, "
            f"kind: {symbol.kind.name}, "
            f'range: "{fmt_range(symbol.range)}", '
            f'selection_range: "{fmt_range(symbol.selection_range)}"'
        )
        if symbol.detail:
            line += f", detail: {yaml_str(symbol.detail)}"
        out.append(line + " }")
        for child in symbol.children or []:
            walk(child, depth + 1)

    for symbol in symbols or []:
        assert isinstance(symbol, DocumentSymbol), (
            f"expected hierarchical document symbols, got {type(symbol).__name__}"
        )
        walk(symbol, 0)
    return out


def decode_semantic_tokens(data, lines, legend):
    """Decode the LSP delta-encoded token array into presenter lines.
    Positions are UTF-16 code units; fixtures are ASCII, where they
    coincide with string indices."""
    out = []
    line, character = 0, 0
    for i in range(0, len(data), 5):
        delta_line, delta_start, length, kind, modifiers = data[i : i + 5]
        line += delta_line
        character = character + delta_start if delta_line == 0 else delta_start
        text = lines[line][character : character + length]
        entry = f'- {{ loc: "{line}:{character}", text: {yaml_str(text)}, kind: {legend.token_types[kind]}'
        names = [
            name
            for bit, name in enumerate(legend.token_modifiers)
            if modifiers & (1 << bit)
        ]
        if names:
            entry += f", modifiers: [{', '.join(names)}]"
        out.append(entry + " }")
    return out


async def present_semantic_tokens(client, uri, source, workspace):
    result = await client.semantic_tokens_full(uri)
    if result is None or not result.data:
        return []
    legend = client.init_result.capabilities.semantic_tokens_provider.legend
    return decode_semantic_tokens(result.data, source.content.split("\n"), legend)


async def present_inlay_hints(client, uri, source, workspace):
    whole_file = Range(
        start=Position(line=0, character=0),
        end=Position(line=source.content.count("\n") + 1, character=0),
    )
    out = []
    for hint in await client.inlay_hints(uri, whole_file) or []:
        label = (
            hint.label
            if isinstance(hint.label, str)
            else "".join(part.value for part in hint.label)
        )
        line = f'- {{ pos: "{fmt_pos(hint.position)}"'
        if hint.kind is not None:
            line += f", kind: {InlayHintKind(hint.kind).name}"
        line += f", label: {yaml_str(label)}"
        if hint.padding_left:
            line += ", padding_left: true"
        if hint.padding_right:
            line += ", padding_right: true"
        out.append(line + " }")
    return out


FEATURES = {
    "document_links": present_document_links,
    "folding_range": present_folding_ranges,
    "document_symbol": present_document_symbols,
    "semantic_tokens": present_semantic_tokens,
    "inlay_hint": present_inlay_hints,
}


def fixture_params():
    for feature in FEATURES:
        base = DATA_DIR / feature
        for path in sorted(base.rglob("*.cpp")):
            rel = path.relative_to(base).as_posix()
            yield pytest.param(
                feature,
                rel,
                id=f"{feature}/{rel}",
                marks=pytest.mark.workspace(feature),
            )


@pytest.mark.parametrize(("feature", "rel"), fixture_params())
async def test_snapshot(feature, rel, client, workspace, request):
    snapshots = SnapshotContext(
        SNAPSHOT_DIR / feature, request.config.getoption("--update-snapshots")
    )

    content = (workspace / rel).read_text()
    if fixture_frontmatter(content, "status") == "unsupported":
        snapshots.check(rel, "UNSUPPORTED")
        return

    source = parse_annotations(content)
    uri, _ = await client.open_and_wait(workspace / rel, text=source.content)
    body = await FEATURES[feature](client, uri, source, workspace)
    client.close(uri)
    snapshots.check(rel, "\n".join(body))
