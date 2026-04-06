"""Fixtures and shared helpers for clice LSP integration tests using pygls LanguageClient."""

import asyncio
import json
import shutil
import subprocess
import sys
from collections.abc import AsyncGenerator
from pathlib import Path
from urllib.parse import unquote

import pytest
from lsprotocol.types import (
    PROGRESS,
    TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS,
    WINDOW_WORK_DONE_PROGRESS_CREATE,
    ClientCapabilities,
    Diagnostic,
    DidOpenTextDocumentParams,
    HoverParams,
    InitializeParams,
    InitializeResult,
    InitializedParams,
    Position,
    ProgressParams,
    PublishDiagnosticsParams,
    TextDocumentIdentifier,
    TextDocumentItem,
    WorkDoneProgressCreateParams,
    WorkspaceFolder,
)
from pygls.lsp.client import BaseLanguageClient


def pytest_addoption(parser: pytest.Parser) -> None:
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

    def __init__(self) -> None:
        super().__init__("clice-test-client", "0.1.0")
        self.diagnostics: dict[str, list[Diagnostic]] = {}
        self.diagnostics_events: dict[str, asyncio.Event] = {}
        self.progress_tokens: list[str] = []
        self.progress_events: list[dict] = []
        self.init_result: InitializeResult | None = None

        @self.feature(TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS)
        def on_diagnostics(params: PublishDiagnosticsParams) -> None:
            raw_uri = params.uri
            normalized = self._normalize_uri(raw_uri)
            diags = list(params.diagnostics)
            # Store under both raw and normalized forms.
            self.diagnostics[raw_uri] = diags
            if raw_uri != normalized:
                self.diagnostics[normalized] = diags
            for key in (raw_uri, normalized):
                if key in self.diagnostics_events:
                    self.diagnostics_events[key].set()

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
    def _normalize_uri(uri: str) -> str:
        """Decode percent-encoded URIs so encoded and unencoded forms match."""
        return unquote(uri)

    def wait_for_diagnostics(self, uri: str) -> asyncio.Event:
        """Get or create an event that fires when diagnostics arrive for uri."""
        uri = self._normalize_uri(uri)
        if uri not in self.diagnostics_events:
            self.diagnostics_events[uri] = asyncio.Event()
        else:
            self.diagnostics_events[uri].clear()
        return self.diagnostics_events[uri]

    async def initialize(self, workspace: Path) -> InitializeResult:
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
        self.init_result = result
        return result

    def open(self, filepath: Path, version: int = 0) -> tuple[str, str]:
        """Open a text document and return (normalized_uri, content).

        Sends the percent-encoded URI on the wire (RFC 3986), but returns
        the normalized (decoded) form for internal lookups.
        """
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

    async def wait_diagnostics(self, uri: str, timeout: float = 30.0) -> None:
        """Wait for diagnostics on the given URI."""
        uri = self._normalize_uri(uri)
        if uri in self.diagnostics:
            return
        event = self.wait_for_diagnostics(uri)
        if uri in self.diagnostics:
            return
        await asyncio.wait_for(event.wait(), timeout=timeout)

    async def open_and_wait(
        self, filepath: Path, timeout: float = 60.0
    ) -> tuple[str, str]:
        """Open a file and trigger compilation by sending a hover request.

        With the pull-based compilation model, compilation is triggered
        by feature requests (hover, completion, etc.) via ensure_compiled(),
        not by didOpen. This method opens the file and sends a hover request
        to trigger compilation, which publishes diagnostics as a side effect.
        """
        uri, content = self.open(filepath)
        event = self.wait_for_diagnostics(uri)
        # Send hover to trigger pull-based compilation (ensure_compiled).
        # This causes the server to compile the file and publish diagnostics.
        await self.text_document_hover_async(
            HoverParams(
                text_document=TextDocumentIdentifier(uri=uri),
                position=Position(line=0, character=0),
            )
        )
        # Wait for diagnostics notification to be processed by the client.
        await asyncio.wait_for(event.wait(), timeout=timeout)
        return uri, content


@pytest.fixture(scope="session")
def executable(request: pytest.FixtureRequest) -> Path:
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
def test_data_dir() -> Path:
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

    # Generate compile_commands.json for header_context (always regenerate
    # because it contains absolute paths).
    hc_dir = data_dir / "header_context"
    hc_main = hc_dir / "main.cpp"
    hc_cdb = hc_dir / "compile_commands.json"
    if hc_main.exists():
        cdb = [
            {
                "directory": hc_dir.as_posix(),
                "file": hc_main.as_posix(),
                "arguments": [
                    "clang++",
                    "-std=c++17",
                    f"-I{hc_dir.as_posix()}",
                    "-fsyntax-only",
                    hc_main.as_posix(),
                ],
            }
        ]
        hc_cdb.write_text(json.dumps(cdb, indent=2))

    # Generate compile_commands.json for include_completion
    ic_dir = data_dir / "include_completion"
    ic_main = ic_dir / "main.cpp"
    ic_cdb = ic_dir / "compile_commands.json"
    if ic_main.exists() and not ic_cdb.exists():
        cdb = [
            {
                "directory": ic_dir.as_posix(),
                "file": ic_main.as_posix(),
                "arguments": [
                    "clang++",
                    "-std=c++17",
                    "-I.",
                    "-fsyntax-only",
                    ic_main.as_posix(),
                ],
            }
        ]
        ic_cdb.write_text(json.dumps(cdb, indent=2))

    return data_dir


def generate_cdb(workspace: Path) -> None:
    """Generate compile_commands.json using CMake with Ninja backend."""
    cmake = shutil.which("cmake")
    if cmake is None:
        raise RuntimeError("cmake executable not found in PATH")
    toolchain = Path(__file__).resolve().parent.parent / "cmake" / "toolchain.cmake"
    cmd = [
        cmake,
        "-G",
        "Ninja",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-S",
        str(workspace),
        "-B",
        str(workspace / "build"),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(f"cmake failed:\n{result.stderr}")


@pytest.fixture
def workspace(request: pytest.FixtureRequest, test_data_dir: Path) -> Path | None:
    """Resolve workspace path from @pytest.mark.workspace("subdir") marker.

    If the workspace contains a CMakeLists.txt, automatically runs cmake
    to generate compile_commands.json. Returns None if no marker is present.
    """
    marker = request.node.get_closest_marker("workspace")
    if marker is None:
        return None
    if not marker.args or not isinstance(marker.args[0], str):
        raise pytest.UsageError(
            "@pytest.mark.workspace requires a string argument, e.g. "
            '@pytest.mark.workspace("modules/hello_world")'
        )
    path = test_data_dir / marker.args[0]
    if (path / "CMakeLists.txt").exists():
        generate_cdb(path)
    # Clean up persisted index/cache so each test starts fresh.
    clice_dir = path / ".clice"
    if clice_dir.exists():
        shutil.rmtree(clice_dir)
    return path


@pytest.fixture
async def client(
    request: pytest.FixtureRequest, executable: Path, workspace: Path | None
):
    """Spawn clice server, auto-initialize if @pytest.mark.workspace is present."""
    config = request.config
    mode = config.getoption("--mode")

    cmd = [str(executable), "--mode", mode]
    if mode == "socket":
        host = config.getoption("--host")
        port = config.getoption("--port")
        cmd += ["--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    if workspace is not None:
        await c.initialize(workspace)

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

    # Dump server stderr warnings for diagnostics.
    try:
        server = getattr(c, "_server", None)
        if server and server.stderr:
            stderr_data = await asyncio.wait_for(server.stderr.read(), timeout=2.0)
            if stderr_data:
                for line in stderr_data.decode("utf-8", errors="replace").splitlines():
                    if "[warn]" in line or "[error]" in line:
                        print(f"[server] {line}", flush=True)
    except Exception:
        pass

    # Stop pygls client (with timeout to avoid hanging)
    try:
        c._stop_event.set()
        for task in c._async_tasks:
            task.cancel()
        await asyncio.sleep(0.1)
    except Exception:
        pass
