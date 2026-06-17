import asyncio
import shutil
import socket
import sys
from pathlib import Path

import pytest

from tests.cdb import generate_cdb, generate_test_data_cdbs
from tests.integration.utils.client import CliceClient


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
    generate_test_data_cdbs(data_dir)
    return data_dir


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
    request: pytest.FixtureRequest,
    executable: Path,
    workspace: Path | None,
):
    """Spawn clice server, auto-initialize if @pytest.mark.workspace is present."""
    config = request.config
    mode = config.getoption("--mode")
    host = config.getoption("--host")

    cmd = [str(executable), "server", "--mode", mode, "--host", host]

    c = CliceClient()
    await c.start_io(*cmd)

    if workspace is not None:
        init_options_marker = request.node.get_closest_marker("init_options")
        init_options = dict(init_options_marker.args[0]) if init_options_marker else {}
        # Force cache_dir into the workspace so .clice/ cleanup prevents stale PCH.
        project = dict(init_options.get("project", {}))
        project.setdefault("cache_dir", str(workspace / ".clice"))
        init_options["project"] = project
        await c.initialize(workspace, initialization_options=init_options)

    yield c

    await shutdown_client(c)


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
async def agentic(
    request: pytest.FixtureRequest,
    executable: Path,
    workspace: Path | None,
):
    """Start a server with agentic TCP port, yield (executable, host, port)."""
    host = "127.0.0.1"
    port = find_free_port()
    cmd = [str(executable), "server", "--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    if workspace is not None:
        init_options_marker = request.node.get_closest_marker("init_options")
        init_options = dict(init_options_marker.args[0]) if init_options_marker else {}
        project = dict(init_options.get("project", {}))
        project.setdefault("cache_dir", str(workspace / ".clice"))
        init_options["project"] = project
        await c.initialize(workspace, initialization_options=init_options)

    yield executable, host, port

    await shutdown_client(c)


async def make_client(executable: Path, workspace: Path) -> CliceClient:
    """Spawn a fresh clice server and initialize it. For multi-session tests."""
    c = CliceClient()
    await c.start_io(str(executable), "server")
    await c.initialize(workspace)
    return c


SANITIZER_MARKERS = (
    "AddressSanitizer",
    "LeakSanitizer",
    "MemorySanitizer",
    "ThreadSanitizer",
    "UndefinedBehaviorSanitizer",
    "==ERROR:",
    "runtime error:",
)


def server_stderr_excerpt(stderr_text: str) -> str:
    interesting = [
        line
        for line in stderr_text.splitlines()
        if "[warn]" in line
        or "[error]" in line
        or "Sanitizer" in line
        or "==ERROR:" in line
        or "runtime error:" in line
    ]
    return "\n".join(interesting[-80:])


async def assert_server_exited_cleanly(server, timeout: float = 10.0) -> None:
    failures: list[str] = []

    if server is None:
        return

    if server.returncode is None:
        try:
            await asyncio.wait_for(server.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            server.kill()
            await server.wait()
            failures.append(f"server did not exit within {timeout:g}s after shutdown")

    print(f"[server] exit code: {server.returncode}", flush=True)

    stderr_text = ""
    if server.stderr:
        try:
            stderr_data = await asyncio.wait_for(server.stderr.read(), timeout=2.0)
            stderr_text = stderr_data.decode("utf-8", errors="replace")
        except Exception as exc:
            failures.append(f"failed to collect server stderr: {exc!r}")

    for line in server_stderr_excerpt(stderr_text).splitlines():
        print(f"[server] {line}", flush=True)

    if server.returncode != 0:
        failures.append(f"server exited with code {server.returncode}")

    if any(marker in stderr_text for marker in SANITIZER_MARKERS):
        failures.append("server stderr contains sanitizer/runtime error output")

    if failures:
        excerpt = server_stderr_excerpt(stderr_text)
        if excerpt:
            failures.append("server stderr excerpt:\n" + excerpt)
        pytest.fail("\n".join(failures))


async def shutdown_client(c: CliceClient) -> None:
    """Gracefully shut down a client, force-kill if needed."""
    server = getattr(c, "_server", None)

    try:
        await asyncio.wait_for(c.shutdown_async(None), timeout=10.0)
    except Exception:
        pass

    try:
        c.exit(None)
    except Exception:
        pass

    try:
        await assert_server_exited_cleanly(server)
    finally:
        try:
            c._stop_event.set()
            for task in c._async_tasks:
                task.cancel()
            await asyncio.sleep(0.1)
        except Exception:
            pass
