"""Client $/cancelRequest reaches the worker (end-to-end cancellation)."""

import asyncio
import uuid

import pytest
from lsprotocol.types import (
    CancelParams,
    CodeActionContext,
    CodeActionParams,
    CompletionParams,
    DefinitionParams,
    DocumentFormattingParams,
    DocumentLinkParams,
    DocumentRangeFormattingParams,
    DocumentSymbolParams,
    FoldingRangeParams,
    FormattingOptions,
    HoverParams,
    InlayHintParams,
    Position,
    Range,
    SemanticTokensParams,
    SignatureHelpParams,
    TextDocumentIdentifier,
)

from tests.tools.compile_commands import write_cdb
from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.workspace import did_change

# Two hundred thousand trivial declarations: slow to parse on any hardware,
# cheap to abandon (the worker polls the stop flag per declaration).
SLOW = "\n".join(f"int v{i};" for i in range(200_000)) + "\n"
LAST_LINE = 199_999


async def cancel_and_expect(client, method, params, timeout=30):
    """Send `method`, cancel it 0.1s later, require a RequestCancelled reply."""
    msg_id = str(uuid.uuid4())
    task = asyncio.ensure_future(
        client.protocol.send_request_async(method, params, msg_id=msg_id)
    )
    await asyncio.sleep(0.1)
    client.protocol.notify("$/cancelRequest", CancelParams(id=msg_id))
    with pytest.raises(Exception) as exc:
        await asyncio.wait_for(task, timeout=timeout)
    assert getattr(exc.value, "code", None) == -32800


async def test_cancelled_completion_replies(executable, tmp_path):
    (tmp_path / "slow.cpp").write_text(SLOW)
    (tmp_path / "tiny.cpp").write_text("int value = 42;\n")
    write_cdb(tmp_path, ["slow.cpp", "tiny.cpp"])

    client = await make_client(executable, tmp_path)
    try:
        uri, _ = client.open(tmp_path / "slow.cpp")

        # Complete at the LAST line: clang truncates the parse at the
        # completion point, so a point at the top would skip the slow body
        # entirely and the request could finish before the cancel arrives.
        await cancel_and_expect(
            client,
            "textDocument/completion",
            CompletionParams(
                text_document=TextDocumentIdentifier(uri=uri),
                position=Position(line=LAST_LINE, character=4),
            ),
        )

        # The server survives the cancellation and still answers. Hover a
        # small file: the slow one would recompile from scratch here and
        # can brush the 30s client timeout on a debug CI runner.
        tiny_uri, _ = client.open(tmp_path / "tiny.cpp")
        hover = await client.hover_at(tiny_uri, 0, 5)
        assert hover is not None
    finally:
        await shutdown_client(client)


async def test_cancelled_signature_help(executable, tmp_path):
    # Same shape as completion (the other stateless build kind): the call
    # site sits past the slow body, so reaching it means a full parse.
    text = SLOW + "void take(int a, int b);\nint use() { return take(1, 2); }\n"
    (tmp_path / "sig.cpp").write_text(text)
    (tmp_path / "tiny.cpp").write_text("int value = 42;\n")
    write_cdb(tmp_path, ["sig.cpp", "tiny.cpp"])

    client = await make_client(executable, tmp_path)
    try:
        uri, _ = client.open(tmp_path / "sig.cpp")

        await cancel_and_expect(
            client,
            "textDocument/signatureHelp",
            SignatureHelpParams(
                text_document=TextDocumentIdentifier(uri=uri),
                position=Position(line=LAST_LINE + 2, character=26),
            ),
        )

        tiny_uri, _ = client.open(tmp_path / "tiny.cpp")
        hover = await client.hover_at(tiny_uri, 0, 5)
        assert hover is not None
    finally:
        await shutdown_client(client)


async def test_cancelled_requests_while_compiling(executable, tmp_path):
    # Every forwarded feature cancelled while the document's compile is
    # still churning through the slow body. All of them must reply with
    # RequestCancelled, and none of them may tear down the shared compile:
    # the closing hover on the SAME file must land with the AST it built.
    (tmp_path / "slow.cpp").write_text(SLOW)
    write_cdb(tmp_path, ["slow.cpp"])

    client = await make_client(executable, tmp_path)
    try:
        uri, _ = client.open(tmp_path / "slow.cpp")
        doc = TextDocumentIdentifier(uri=uri)
        head = Range(
            start=Position(line=0, character=0), end=Position(line=10, character=0)
        )
        fmt = FormattingOptions(tab_size=4, insert_spaces=True)

        requests = [
            (
                "textDocument/hover",
                HoverParams(text_document=doc, position=Position(line=0, character=4)),
            ),
            (
                "textDocument/definition",
                DefinitionParams(
                    text_document=doc, position=Position(line=0, character=4)
                ),
            ),
            ("textDocument/documentSymbol", DocumentSymbolParams(text_document=doc)),
            (
                "textDocument/semanticTokens/full",
                SemanticTokensParams(text_document=doc),
            ),
            ("textDocument/foldingRange", FoldingRangeParams(text_document=doc)),
            ("textDocument/inlayHint", InlayHintParams(text_document=doc, range=head)),
            (
                "textDocument/codeAction",
                CodeActionParams(
                    text_document=doc,
                    range=head,
                    context=CodeActionContext(diagnostics=[]),
                ),
            ),
            ("textDocument/documentLink", DocumentLinkParams(text_document=doc)),
            (
                "textDocument/formatting",
                DocumentFormattingParams(text_document=doc, options=fmt),
            ),
            (
                "textDocument/rangeFormatting",
                DocumentRangeFormattingParams(
                    text_document=doc, range=head, options=fmt
                ),
            ),
        ]
        for method, params in requests:
            await cancel_and_expect(client, method, params)

        hover = await client.hover_at(uri, 0, 4, timeout=120)
        assert hover is not None
    finally:
        await shutdown_client(client)


async def test_edit_supersedes_compile(executable, tmp_path):
    # An edit mid-compile abandons the stale parse end-to-end: the request
    # that launched it resolves promptly (null — the editor re-queries
    # after an edit), and the next request answers on the new content.
    (tmp_path / "edited.cpp").write_text(SLOW)
    write_cdb(tmp_path, ["edited.cpp"])

    client = await make_client(executable, tmp_path)
    try:
        uri, _ = client.open(tmp_path / "edited.cpp")

        first = asyncio.ensure_future(
            client.text_document_hover_async(
                HoverParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=0, character=4),
                )
            )
        )
        await asyncio.sleep(0.3)
        did_change(client, uri, version=1, text="int fixed;\n")

        assert await asyncio.wait_for(first, timeout=10) is None

        hover = await client.hover_at(uri, 0, 4)
        assert hover is not None
    finally:
        await shutdown_client(client)
