"""CliceClient — enhanced LSP client for integration testing."""

import asyncio
from pathlib import Path
from urllib.parse import unquote

from lsprotocol.types import (
    PROGRESS,
    TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS,
    WINDOW_LOG_MESSAGE,
    WINDOW_WORK_DONE_PROGRESS_CREATE,
    ClientCapabilities,
    CodeActionContext,
    CodeActionParams,
    CompletionParams,
    DefinitionParams,
    Diagnostic,
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    DocumentLinkParams,
    DocumentSymbolParams,
    FoldingRangeParams,
    HoverParams,
    InlayHintParams,
    InitializeParams,
    InitializeResult,
    InitializedParams,
    LogMessageParams,
    Position,
    ProgressParams,
    PublishDiagnosticsParams,
    Range,
    ReferenceContext,
    ReferenceParams,
    SemanticTokensParams,
    SignatureHelpParams,
    TextDocumentIdentifier,
    TextDocumentItem,
    WorkDoneProgressCreateParams,
    WorkspaceFolder,
)
from pygls.lsp.client import BaseLanguageClient


class CliceClient(BaseLanguageClient):
    """Language client that tracks server-sent notifications and provides
    convenience methods for common LSP operations."""

    def __init__(self) -> None:
        super().__init__("clice-test-client", "0.1.0")
        self.diagnostics: dict[str, list[Diagnostic]] = {}
        self.diagnostics_events: dict[str, asyncio.Event] = {}
        self.log_messages: list[LogMessageParams] = []
        self.progress_tokens: list[str] = []
        self.progress_events: list[dict] = []
        self.init_result: InitializeResult | None = None

        @self.feature(TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS)
        def on_diagnostics(params: PublishDiagnosticsParams) -> None:
            raw_uri = params.uri
            normalized = self._normalize_uri(raw_uri)
            diags = list(params.diagnostics)
            self.diagnostics[raw_uri] = diags
            if raw_uri != normalized:
                self.diagnostics[normalized] = diags
            for key in (raw_uri, normalized):
                if key in self.diagnostics_events:
                    self.diagnostics_events[key].set()

        @self.feature(WINDOW_LOG_MESSAGE)
        def on_log_message(params: LogMessageParams) -> None:
            self.log_messages.append(params)

        @self.feature(WINDOW_WORK_DONE_PROGRESS_CREATE)
        def on_create_progress(params: WorkDoneProgressCreateParams) -> None:
            token = str(params.token) if isinstance(params.token, int) else params.token
            self.progress_tokens.append(token)
            return None

        @self.feature(PROGRESS)
        def on_progress(params: ProgressParams) -> None:
            token = str(params.token) if isinstance(params.token, int) else params.token
            self.progress_events.append({"token": token, "value": params.value})

    # ── URI helpers ──────────────────────────────────────────────────

    @staticmethod
    def _normalize_uri(uri: str) -> str:
        return unquote(uri)

    def path_to_uri(self, filepath: Path) -> str:
        return self._normalize_uri(filepath.as_uri())

    # ── Lifecycle ────────────────────────────────────────────────────

    async def initialize(
        self,
        workspace: Path,
        *,
        initialization_options: dict | None = None,
    ) -> InitializeResult:
        params = InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=workspace.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=workspace.as_uri(), name="test")],
        )
        if initialization_options is not None:
            params.initialization_options = initialization_options
        result = await self.initialize_async(params)
        self.initialized(InitializedParams())
        self.init_result = result
        return result

    # ── Document operations ──────────────────────────────────────────

    def open(self, filepath: Path, version: int = 0) -> tuple[str, str]:
        """Open a text document. Returns (normalized_uri, content)."""
        content = filepath.read_bytes().decode("utf-8")
        wire_uri = filepath.as_uri()
        self.text_document_did_open(
            DidOpenTextDocumentParams(
                text_document=TextDocumentItem(
                    uri=wire_uri, language_id="cpp", version=version, text=content
                )
            )
        )
        return self._normalize_uri(wire_uri), content

    def close(self, uri: str) -> None:
        """Close a text document."""
        self.text_document_did_close(
            DidCloseTextDocumentParams(text_document=TextDocumentIdentifier(uri=uri))
        )

    # ── Diagnostics ──────────────────────────────────────────────────

    def wait_for_diagnostics(self, uri: str) -> asyncio.Event:
        uri = self._normalize_uri(uri)
        if uri not in self.diagnostics_events:
            self.diagnostics_events[uri] = asyncio.Event()
        else:
            self.diagnostics_events[uri].clear()
        return self.diagnostics_events[uri]

    async def wait_diagnostics(self, uri: str, timeout: float = 30.0) -> None:
        uri = self._normalize_uri(uri)
        if uri in self.diagnostics:
            return
        event = self.wait_for_diagnostics(uri)
        if uri in self.diagnostics:
            return
        await asyncio.wait_for(event.wait(), timeout=timeout)

    # ── Compile & wait ───────────────────────────────────────────────

    async def open_and_wait(
        self, filepath: Path, timeout: float = 60.0
    ) -> tuple[str, str]:
        """Open a file and trigger compilation via hover. Waits for diagnostics."""
        uri, content = self.open(filepath)
        event = self.wait_for_diagnostics(uri)
        await self.text_document_hover_async(
            HoverParams(
                text_document=TextDocumentIdentifier(uri=uri),
                position=Position(line=0, character=0),
            )
        )
        await asyncio.wait_for(event.wait(), timeout=timeout)
        return uri, content

    # ── Feature request shortcuts ────────────────────────────────────

    async def hover_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send hover request at given position."""
        return await asyncio.wait_for(
            self.text_document_hover_async(
                HoverParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                )
            ),
            timeout=timeout,
        )

    async def definition_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send go-to-definition request at given position."""
        return await asyncio.wait_for(
            self.text_document_definition_async(
                DefinitionParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                )
            ),
            timeout=timeout,
        )

    async def references_at(
        self,
        uri: str,
        line: int,
        character: int,
        *,
        include_declaration: bool = True,
        timeout: float = 30.0,
    ):
        """Send find-references request at given position."""
        return await asyncio.wait_for(
            self.text_document_references_async(
                ReferenceParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                    context=ReferenceContext(include_declaration=include_declaration),
                )
            ),
            timeout=timeout,
        )

    async def completion_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send completion request at given position."""
        return await asyncio.wait_for(
            self.text_document_completion_async(
                CompletionParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                )
            ),
            timeout=timeout,
        )

    async def signature_help_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send signature help request at given position."""
        return await asyncio.wait_for(
            self.text_document_signature_help_async(
                SignatureHelpParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                )
            ),
            timeout=timeout,
        )

    async def document_symbols(self, uri: str, *, timeout: float = 30.0):
        """Send document symbol request."""
        return await asyncio.wait_for(
            self.text_document_document_symbol_async(
                DocumentSymbolParams(text_document=TextDocumentIdentifier(uri=uri))
            ),
            timeout=timeout,
        )

    async def folding_ranges(self, uri: str, *, timeout: float = 30.0):
        """Send folding range request."""
        return await asyncio.wait_for(
            self.text_document_folding_range_async(
                FoldingRangeParams(text_document=TextDocumentIdentifier(uri=uri))
            ),
            timeout=timeout,
        )

    async def semantic_tokens_full(self, uri: str, *, timeout: float = 30.0):
        """Send semantic tokens (full) request."""
        return await asyncio.wait_for(
            self.text_document_semantic_tokens_full_async(
                SemanticTokensParams(text_document=TextDocumentIdentifier(uri=uri))
            ),
            timeout=timeout,
        )

    async def inlay_hints(self, uri: str, range_: Range, *, timeout: float = 30.0):
        """Send inlay hint request for given range."""
        return await asyncio.wait_for(
            self.text_document_inlay_hint_async(
                InlayHintParams(
                    text_document=TextDocumentIdentifier(uri=uri), range=range_
                )
            ),
            timeout=timeout,
        )

    async def code_actions(
        self,
        uri: str,
        range_: Range,
        diagnostics=None,
        *,
        timeout: float = 30.0,
    ):
        """Send code action request."""
        return await asyncio.wait_for(
            self.text_document_code_action_async(
                CodeActionParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    range=range_,
                    context=CodeActionContext(diagnostics=diagnostics or []),
                )
            ),
            timeout=timeout,
        )

    async def document_links(self, uri: str, *, timeout: float = 30.0):
        """Send document link request."""
        return await asyncio.wait_for(
            self.text_document_document_link_async(
                DocumentLinkParams(text_document=TextDocumentIdentifier(uri=uri))
            ),
            timeout=timeout,
        )

    # ── Extension protocol ───────────────────────────────────────────

    async def query_context(self, uri: str, *, timeout: float = 30.0):
        """Send clice/queryContext extension request."""
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/queryContext", {"uri": uri}),
            timeout=timeout,
        )

    async def current_context(self, uri: str, *, timeout: float = 30.0):
        """Send clice/currentContext extension request."""
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/currentContext", {"uri": uri}),
            timeout=timeout,
        )

    async def switch_context(
        self, uri: str, context_uri: str, *, timeout: float = 30.0
    ):
        """Send clice/switchContext extension request."""
        return await asyncio.wait_for(
            self.protocol.send_request_async(
                "clice/switchContext", {"uri": uri, "contextUri": context_uri}
            ),
            timeout=timeout,
        )
