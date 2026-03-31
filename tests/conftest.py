"""Fixtures and shared helpers for clice LSP integration tests using pygls LanguageClient."""

import json
import asyncio
import sys
from pathlib import Path

import pytest
import pytest_asyncio
from lsprotocol.types import (
    PROGRESS,
    TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS,
    WINDOW_WORK_DONE_PROGRESS_CREATE,
    ClientCapabilities,
    Diagnostic,
    DidOpenTextDocumentParams,
    InitializeParams,
    InitializedParams,
    ProgressParams,
    PublishDiagnosticsParams,
    TextDocumentItem,
    WorkDoneProgressCreateParams,
    WorkspaceFolder,
)
from pygls.lsp.client import BaseLanguageClient


def pytest_addoption(parser: pytest.Parser):
    parser.addoption(
        "--executable",
        required=False,
        help="Path to the clice executable.",
    )
    parser.addoption(
        "--mode",
        type=str,
        choices=["pipe", "socket"],
        default="pipe",
        help="The connection mode to use.",
    )
    parser.addoption(
        "--host",
        type=str,
        default="127.0.0.1",
        help="The host to connect to (default: 127.0.0.1)",
    )
    parser.addoption(
        "--port",
        type=int,
        default=50051,
        help="The port to connect to",
    )


class CliceClient(BaseLanguageClient):
    """Language client that tracks server-sent notifications."""

    def __init__(self):
        super().__init__("clice-test-client", "0.1.0")
        self.diagnostics: dict[str, list[Diagnostic]] = {}
        self.diagnostics_events: dict[str, asyncio.Event] = {}
        self.progress_tokens: list[str] = []
        self.progress_events: list[dict] = []

        @self.feature(TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS)
        def on_diagnostics(params: PublishDiagnosticsParams):
            self.diagnostics[params.uri] = list(params.diagnostics)
            if params.uri in self.diagnostics_events:
                self.diagnostics_events[params.uri].set()

        @self.feature(WINDOW_WORK_DONE_PROGRESS_CREATE)
        def on_create_progress(params: WorkDoneProgressCreateParams):
            token = str(params.token) if isinstance(params.token, int) else params.token
            self.progress_tokens.append(token)
            return None

        @self.feature(PROGRESS)
        def on_progress(params: ProgressParams):
            token = str(params.token) if isinstance(params.token, int) else params.token
            self.progress_events.append({"token": token, "value": params.value})

    def wait_for_diagnostics(self, uri: str) -> asyncio.Event:
        """Get or create an event that fires when diagnostics arrive for uri."""
        if uri not in self.diagnostics_events:
            self.diagnostics_events[uri] = asyncio.Event()
        else:
            self.diagnostics_events[uri].clear()
        return self.diagnostics_events[uri]

    async def initialize(self, workspace: Path):
        """Initialize the LSP server with a workspace folder and return the result."""
        result = await self.initialize_async(
            InitializeParams(
                capabilities=ClientCapabilities(),
                root_uri=workspace.as_uri(),
                workspace_folders=[
                    WorkspaceFolder(uri=workspace.as_uri(), name="test")
                ],
            )
        )
        self.initialized(InitializedParams())
        return result

    def open(self, filepath: Path, version: int = 0):
        """Open a text document and return (uri, content)."""
        content = filepath.read_text(encoding="utf-8")
        uri = filepath.as_uri()
        self.text_document_did_open(
            DidOpenTextDocumentParams(
                text_document=TextDocumentItem(
                    uri=uri, language_id="cpp", version=version, text=content
                )
            )
        )
        return uri, content

    async def wait_diagnostics(self, uri: str, timeout: float = 30.0):
        """Wait for diagnostics on the given URI."""
        event = self.wait_for_diagnostics(uri)
        await asyncio.wait_for(event.wait(), timeout=timeout)

    async def open_and_wait(self, filepath: Path, timeout: float = 60.0):
        """Open a file and wait for compilation diagnostics."""
        uri = filepath.as_uri()
        event = self.wait_for_diagnostics(uri)
        _, content = self.open(filepath)
        await asyncio.wait_for(event.wait(), timeout=timeout)
        return uri, content


@pytest.fixture(scope="session")
def executable(request) -> Path:
    exe = request.config.getoption("--executable")
    if not exe:
        pytest.skip("--executable not provided")

    path = Path(exe)
    if sys.platform.startswith("win") and path.suffix.lower() != ".exe":
        path_exe = path.with_name(path.name + ".exe")
        if path_exe.exists() or not path.exists():
            path = path_exe

    if not path.exists():
        pytest.exit(
            f"Error: clice executable not found at '{exe}'. "
            "Please ensure the path is correct.",
            returncode=64,
        )
    return path.resolve()


@pytest.fixture(scope="session")
def test_data_dir():
    path = Path(__file__).parent / "data"
    data_dir = path.resolve()

    # Generate compile_commands.json for hello_world
    hw_dir = data_dir / "hello_world"
    main_cpp = hw_dir / "main.cpp"
    cdb_path = hw_dir / "compile_commands.json"
    if main_cpp.exists() and not cdb_path.exists():
        cdb = [
            {
                "directory": hw_dir.as_posix(),
                "file": main_cpp.as_posix(),
                "arguments": [
                    "clang++",
                    "-std=c++17",
                    "-fsyntax-only",
                    main_cpp.as_posix(),
                ],
            }
        ]
        cdb_path.write_text(json.dumps(cdb, indent=2))

    return data_dir


@pytest_asyncio.fixture
async def client(request, executable: Path, test_data_dir: Path):
    """Spawn clice server, yield pygls client, then shutdown+exit."""
    config = request.config
    mode = config.getoption("--mode")

    cmd = [str(executable), "--mode", mode]
    if mode == "socket":
        host = config.getoption("--host")
        port = config.getoption("--port")
        cmd += ["--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    yield c

    # Graceful shutdown
    try:
        await asyncio.wait_for(c.shutdown_async(None), timeout=3.0)
    except Exception:
        pass
    try:
        c.exit(None)
    except Exception:
        pass

    # Wait briefly, then force-kill if still running
    await asyncio.sleep(0.3)
    if hasattr(c, "_server") and c._server is not None and c._server.returncode is None:
        c._server.kill()

    # Stop pygls client (with timeout to avoid hanging)
    try:
        c._stop_event.set()
        for task in c._async_tasks:
            task.cancel()
        await asyncio.sleep(0.1)
    except Exception:
        pass
