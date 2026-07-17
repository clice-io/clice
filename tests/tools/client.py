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
    CompletionContext,
    CompletionParams,
    CompletionTriggerKind,
    DeclarationParams,
    DefinitionParams,
    Diagnostic,
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    DocumentFormattingParams,
    DocumentLinkParams,
    DocumentRangeFormattingParams,
    DocumentSymbolParams,
    FoldingRangeParams,
    FormattingOptions,
    HoverParams,
    ImplementationParams,
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
    TypeDefinitionParams,
    TextDocumentItem,
    WorkDoneProgressCreateParams,
    WorkspaceFolder,
)
from pygls.lsp.client import BaseLanguageClient

# Sanitizer/crash fingerprints scanned in server stderr. Detection happens
# incrementally in the pump: a mid-session report (e.g. relayed from a
# crashed worker) must survive the retention cap's eviction.
SANITIZER_MARKERS = (
    "AddressSanitizer",
    "LeakSanitizer",
    "MemorySanitizer",
    "ThreadSanitizer",
    "UndefinedBehaviorSanitizer",
    "==ERROR:",
    "runtime error:",
)

SANITIZER_MARKER_BYTES = tuple(m.encode() for m in SANITIZER_MARKERS)


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
        self.workspace: Path | None = None
        self.stderr_chunks: list[bytes] = []
        self.stderr_retained = 0
        self.stderr_pump: asyncio.Task | None = None
        self.stderr_drained_from_start = True
        self.stderr_marker_hit: bytes | None = None
        self.stderr_scan_carry = b""

        @self.feature(TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS)
        def on_diagnostics(params: PublishDiagnosticsParams) -> None:
            raw_uri = params.uri
            normalized = self.normalize_uri(raw_uri)
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

    @staticmethod
    def normalize_uri(uri: str) -> str:
        return unquote(uri)

    def path_to_uri(self, filepath: Path) -> str:
        return self.normalize_uri(filepath.as_uri())

    async def initialize(
        self,
        workspace: Path,
        *,
        initialization_options: dict | None = None,
    ) -> InitializeResult:
        if initialization_options is None:
            initialization_options = {}
        project = dict(initialization_options.get("project", {}))
        # Force cache_dir into the workspace so .clice/ cleanup prevents
        # stale PCH.
        project["cache_dir"] = str(workspace / ".clice")
        # One worker of each kind is enough for tests and halves the
        # per-test process-spawn cost (5 -> 3 processes), which dominates
        # suite time on macOS Debug. Tests needing more pass their own
        # counts via initialization_options.
        project.setdefault("stateless_worker_count", 1)
        project.setdefault("stateful_worker_count", 1)
        initialization_options["project"] = project
        # Disable the stat-polling loops: tests drive ticks deterministically
        # through the clice/internal/poll hook instead.
        tracker = dict(initialization_options.get("tracker", {}))
        tracker.setdefault("cdb_poll_seconds", 0)
        tracker.setdefault("workspace_poll_seconds", 0)
        initialization_options["tracker"] = tracker

        params = InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=workspace.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=workspace.as_uri(), name="test")],
        )
        params.initialization_options = initialization_options
        result = await self.initialize_async(params)
        self.initialized(InitializedParams())
        self.init_result = result
        self.workspace = workspace
        return result

    # Single home for the pygls internals these wrap; tests must not poke
    # at _server/_stop_event/_async_tasks directly.

    async def start_io(self, *args, drain_stderr: bool = True, **kwargs) -> None:
        await super().start_io(*args, **kwargs)
        # The server treats stderr as best-effort, but a client that never
        # reads it forfeits the full mirror (lines are dropped once the
        # pipe fills). Drain it continuously so long tests keep the whole
        # transcript; backpressure tests opt out to play the hostile client.
        self.stderr_drained_from_start = drain_stderr
        if drain_stderr and self._server and self._server.stderr:
            self.spawn_stderr_pump()

    def spawn_stderr_pump(self) -> None:
        """Start the continuous stderr drain if none is running.
        Backpressure tests spawn it late: asyncio pauses an undrained
        stderr transport at its buffer limit, and Process.wait() cannot
        observe pipe EOF — hence process exit — until reading resumes."""
        if self.stderr_pump is not None and not self.stderr_pump.done():
            return
        self.stderr_pump = asyncio.get_running_loop().create_task(
            self.pump_server_stderr()
        )
        self._async_tasks.append(self.stderr_pump)

    # Retention cap for drained stderr: long stress runs mirror the whole
    # server log, and the teardown scans only need the tail (sanitizer
    # reports and crash text arrive at exit).
    STDERR_RETAIN_BYTES = 8 * 1024 * 1024

    async def pump_server_stderr(self) -> None:
        assert self._server is not None and self._server.stderr is not None
        while True:
            data = await self._server.stderr.read(65536)
            if not data:
                return
            self.scan_for_markers(data)
            self.stderr_chunks.append(data)
            self.stderr_retained += len(data)
            while (
                self.stderr_retained > self.STDERR_RETAIN_BYTES
                and len(self.stderr_chunks) > 1
            ):
                self.stderr_retained -= len(self.stderr_chunks.pop(0))

    def scan_for_markers(self, data: bytes) -> None:
        """Latch the earliest sanitizer fingerprint and keep appending
        context from later reads; the carry covers markers split across
        read boundaries."""
        if self.stderr_marker_hit is not None:
            if len(self.stderr_marker_hit) < 4096:
                self.stderr_marker_hit += data[: 4096 - len(self.stderr_marker_hit)]
            return
        window = self.stderr_scan_carry + data
        hits = [at for m in SANITIZER_MARKER_BYTES if (at := window.find(m)) >= 0]
        if hits:
            self.stderr_marker_hit = window[min(hits) : min(hits) + 4096]
            return
        self.stderr_scan_carry = window[-64:]

    def drained_stderr(self) -> bytes:
        return b"".join(self.stderr_chunks)

    @property
    def server(self) -> asyncio.subprocess.Process | None:
        """The spawned server process, if started via start_io."""
        return self._server

    def kill_server(self) -> None:
        """Force-kill the server process, simulating a crash."""
        assert self._server is not None, "no server process to kill"
        self._server.kill()

    async def stop_io(self) -> None:
        """Tear down client-side IO tasks without contacting the server."""
        self._stop_event.set()
        for task in self._async_tasks:
            task.cancel()
        # Wait the cancellations out so no task outlives the test teardown.
        await asyncio.gather(*self._async_tasks, return_exceptions=True)

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
        return self.normalize_uri(wire_uri), content

    def close(self, uri: str) -> None:
        """Close a text document."""
        self.text_document_did_close(
            DidCloseTextDocumentParams(text_document=TextDocumentIdentifier(uri=uri))
        )

    def wait_for_diagnostics(self, uri: str) -> asyncio.Event:
        uri = self.normalize_uri(uri)
        if uri not in self.diagnostics_events:
            self.diagnostics_events[uri] = asyncio.Event()
        else:
            self.diagnostics_events[uri].clear()
        return self.diagnostics_events[uri]

    async def wait_diagnostics(self, uri: str, timeout: float = 30.0) -> None:
        uri = self.normalize_uri(uri)
        if uri in self.diagnostics:
            return
        event = self.wait_for_diagnostics(uri)
        if uri in self.diagnostics:
            return
        await asyncio.wait_for(event.wait(), timeout=timeout)

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

    async def declaration_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send go-to-declaration request at given position."""
        return await asyncio.wait_for(
            self.text_document_declaration_async(
                DeclarationParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                )
            ),
            timeout=timeout,
        )

    async def implementation_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send go-to-implementation request at given position."""
        return await asyncio.wait_for(
            self.text_document_implementation_async(
                ImplementationParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                )
            ),
            timeout=timeout,
        )

    async def type_definition_at(
        self, uri: str, line: int, character: int, *, timeout: float = 30.0
    ):
        """Send go-to-type-definition request at given position."""
        return await asyncio.wait_for(
            self.text_document_type_definition_async(
                TypeDefinitionParams(
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
        self,
        uri: str,
        line: int,
        character: int,
        *,
        trigger_character: str | None = None,
        timeout: float = 30.0,
    ):
        """Send completion request at given position."""
        context = None
        if trigger_character is not None:
            context = CompletionContext(
                trigger_kind=CompletionTriggerKind.TriggerCharacter,
                trigger_character=trigger_character,
            )
        return await asyncio.wait_for(
            self.text_document_completion_async(
                CompletionParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    position=Position(line=line, character=character),
                    context=context,
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

    async def format_document(self, uri: str, *, timeout: float = 30.0):
        return await asyncio.wait_for(
            self.text_document_formatting_async(
                DocumentFormattingParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    options=FormattingOptions(tab_size=4, insert_spaces=True),
                )
            ),
            timeout=timeout,
        )

    async def format_range(self, uri: str, range_: Range, *, timeout: float = 30.0):
        return await asyncio.wait_for(
            self.text_document_range_formatting_async(
                DocumentRangeFormattingParams(
                    text_document=TextDocumentIdentifier(uri=uri),
                    range=range_,
                    options=FormattingOptions(tab_size=4, insert_spaces=True),
                )
            ),
            timeout=timeout,
        )

    async def query_context(
        self, uri: str, *, offset: int | None = None, timeout: float = 30.0
    ):
        """Send clice/queryContext extension request."""
        params = {"uri": uri}
        if offset is not None:
            params["offset"] = offset
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/queryContext", params),
            timeout=timeout,
        )

    async def current_context(self, uri: str, *, timeout: float = 30.0):
        """Send clice/currentContext extension request."""
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/currentContext", {"uri": uri}),
            timeout=timeout,
        )

    async def switch_context(
        self,
        uri: str,
        context_uri: str,
        *,
        occurrence: int | None = None,
        command_hash: str | None = None,
        epoch: int | None = None,
        timeout: float = 30.0,
    ):
        """Send clice/switchContext extension request."""
        params = {"uri": uri, "contextUri": context_uri}
        if occurrence is not None:
            params["occurrence"] = occurrence
        if command_hash is not None:
            params["commandHash"] = command_hash
        if epoch is not None:
            params["epoch"] = epoch
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/switchContext", params),
            timeout=timeout,
        )

    async def poll(self, loop: str, *, timeout: float = 60.0):
        """Send clice/internal/poll (test hook): run one tracker tick and
        apply its effects synchronously. `loop` is "cdb" or "workspace"."""
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/internal/poll", {"loop": loop}),
            timeout=timeout,
        )

    async def stats(self, *, timeout: float = 60.0):
        """Send clice/internal/stats (test hook): ownership gauges for
        memory-lifecycle assertions. Returns the raw result object."""
        return await asyncio.wait_for(
            self.protocol.send_request_async("clice/internal/stats", {}),
            timeout=timeout,
        )
