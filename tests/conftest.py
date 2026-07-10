import asyncio
import os
import shutil
import socket
import sys
from pathlib import Path

import pytest

from tests.cdb import generate_cdb, generate_test_data_cdbs
from tests.integration.utils.client import CliceClient
from tests.integration.utils.assertions import assert_no_anomaly


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """Store test outcome so fixtures can detect failures during teardown."""
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)


def pytest_configure(config: pytest.Config) -> None:
    # Generate test-data CDBs once in the xdist controller; workers would
    # race writing the same compile_commands.json files.
    if not hasattr(config, "workerinput"):
        generate_test_data_cdbs((Path(__file__).parent / "data").resolve())


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(items: list[pytest.Item]) -> None:
    # Tests sharing a @workspace directory mutate it (.clice cleanup, cmake
    # regeneration), so pin each directory's tests to one xdist worker.
    # tryfirst: must add the mark before xdist's own hook folds xdist_group
    # into the nodeid for the loadgroup scheduler.
    for item in items:
        marker = item.get_closest_marker("workspace")
        if marker and marker.args:
            item.add_marker(pytest.mark.xdist_group(marker.args[0]))


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--executable",
        required=False,
        help="Path to the clice executable.",
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
    # CDB generation happens in pytest_configure (controller side).
    return (Path(__file__).parent / "data").resolve()


@pytest.fixture
def workspace(request: pytest.FixtureRequest, test_data_dir: Path):
    """Resolve workspace path from @pytest.mark.workspace("subdir") marker.

    If the workspace contains a CMakeLists.txt, automatically runs cmake
    to generate compile_commands.json. Returns None if no marker is present.
    """
    marker = request.node.get_closest_marker("workspace")
    if marker is None:
        yield None
        return
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
    yield path
    # Post-test cleanup: drop the cache generated during the test so static
    # test-data directories don't accumulate state.
    if clice_dir.exists():
        shutil.rmtree(clice_dir, ignore_errors=True)


def build_init_options(request: pytest.FixtureRequest, workspace: Path) -> dict:
    """Initialization options from @pytest.mark.init_options plus test defaults."""
    marker = request.node.get_closest_marker("init_options")
    init_options = dict(marker.args[0]) if marker else {}
    project = dict(init_options.get("project", {}))
    # Force cache_dir into the workspace so .clice/ cleanup prevents stale PCH.
    project["cache_dir"] = str(workspace / ".clice")
    # One worker of each kind is enough for tests and halves the per-test
    # process-spawn cost (5 -> 3 processes), which dominates suite time on
    # macOS Debug. Tests needing more override via @pytest.mark.init_options.
    project.setdefault("stateless_worker_count", 1)
    project.setdefault("stateful_worker_count", 1)
    init_options["project"] = project
    return init_options


@pytest.fixture
async def client(
    request: pytest.FixtureRequest,
    executable: Path,
    workspace: Path | None,
):
    """Spawn clice server, auto-initialize if @pytest.mark.workspace is present."""
    cmd = [str(executable), "serve"]

    c = CliceClient()
    await c.start_io(*cmd)

    if workspace is not None:
        init_options = build_init_options(request, workspace)
        await c.initialize(workspace, initialization_options=init_options)

    yield c

    test_failed = (
        getattr(request.node, "rep_call", None) is not None
        and request.node.rep_call.failed
    )
    # The anomaly gate must run even when shutdown itself fails — a crashed
    # server is exactly when the anomaly evidence matters most.
    try:
        await shutdown_client(c, verbose=test_failed)
    finally:
        check_no_anomaly(request, c)


def check_no_anomaly(request: pytest.FixtureRequest, c: CliceClient) -> None:
    """Teardown gate: anomalies are internal clice bugs — every test session
    must end without one. Tests that intentionally trigger anomalies opt out
    with @pytest.mark.allow_anomaly and assert on them explicitly."""
    if request.node.get_closest_marker("allow_anomaly") is not None:
        return
    assert_no_anomaly(c, c.workspace)


next_port_offset = 0


def find_free_port() -> int:
    """Pick a port from a per-xdist-worker range.

    bind(0) draws from the kernel's shared pool: two concurrent xdist
    workers can grab the same port in the close-then-rebind gap. Disjoint
    per-worker ranges (below the ephemeral range) remove that race; the
    advancing offset avoids immediately reusing a just-released port.
    """
    global next_port_offset
    worker = os.environ.get("PYTEST_XDIST_WORKER", "gw0")
    suffix = worker.removeprefix("gw")
    index = int(suffix) if suffix.isdigit() else 0
    base = 21000 + index * 100
    for _ in range(100):
        port = base + next_port_offset
        next_port_offset = (next_port_offset + 1) % 100
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError(f"no free port in range {base}-{base + 99}")


@pytest.fixture
async def agentic(
    request: pytest.FixtureRequest,
    executable: Path,
    workspace: Path | None,
):
    """Start a server with agentic TCP port, yield (executable, host, port)."""
    host = "127.0.0.1"
    port = find_free_port()
    cmd = [str(executable), "serve", "--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    if workspace is not None:
        init_options = build_init_options(request, workspace)
        await c.initialize(workspace, initialization_options=init_options)

    yield executable, host, port

    try:
        await shutdown_client(c)
    finally:
        check_no_anomaly(request, c)


async def make_client(executable: Path, workspace: Path) -> CliceClient:
    """Spawn a fresh clice server and initialize it. For multi-session tests."""
    c = CliceClient()
    await c.start_io(str(executable), "serve")
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


async def shutdown_client(c: CliceClient, *, verbose: bool = False) -> None:
    """Gracefully shut down a client, force-kill if needed."""
    try:
        await asyncio.wait_for(c.shutdown_async(None), timeout=10.0)
    except Exception:
        pass

    try:
        c.exit(None)
    except Exception:
        pass

    if verbose and c.log_messages:
        for msg in c.log_messages:
            level = {1: "ERROR", 2: "WARN", 3: "INFO", 4: "LOG"}.get(msg.type, "?")
            print(f"[logMessage/{level}] {msg.message}", flush=True)

    try:
        await assert_server_exited_cleanly(c.server)
    finally:
        try:
            await c.stop_io()
            await asyncio.sleep(0.1)
        except Exception:
            pass
