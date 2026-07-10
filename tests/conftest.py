import shutil
import sys
from pathlib import Path

import pytest

from tests.tools.compile_commands import generate_cdb, generate_test_data_cdbs
from tests.tools.client import CliceClient
from tests.tools.lifecycle import (
    check_no_anomaly,
    find_free_port,
    shutdown_client,
)


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


def marker_init_options(request: pytest.FixtureRequest) -> dict:
    """Initialization options from @pytest.mark.init_options, if present."""
    marker = request.node.get_closest_marker("init_options")
    return dict(marker.args[0]) if marker else {}


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
        await c.initialize(
            workspace, initialization_options=marker_init_options(request)
        )

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
        await c.initialize(
            workspace, initialization_options=marker_init_options(request)
        )

    yield executable, host, port

    try:
        await shutdown_client(c)
    finally:
        check_no_anomaly(request, c)
