import os
import pytest
import pytest_asyncio
from pathlib import Path
from .fixtures.client import LSPClient


def pytest_addoption(parser: pytest.Parser):
    parser.addoption(
        "--executable",
        required=False,
        help="Path to the of the clice executable.",
    )

    CONNECTION_MODES = ["pipe", "socket"]
    parser.addoption(
        "--mode",
        type=str,
        choices=CONNECTION_MODES,
        default="pipe",
        help=f"The connection mode to use. Must be one of: {', '.join(CONNECTION_MODES)})",
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

    parser.addoption(
        "--resource-dir",
        required=False,
        help="Path to the of the clang resource directory.",
    )


@pytest.fixture(scope="session")
def executable(request) -> Path | None:
    executable = request.config.getoption("--executable")
    if not executable:
        return None

    path = Path(executable)
    if not path.exists():
        pytest.exit(
            f"Error: 'clice' executable not found at '{executable}'. "
            "Please ensure the path is correct and the file exists.",
            returncode=64,
        )

    return path.resolve()


@pytest.fixture(scope="session")
def resource_dir(request) -> Path | None:
    path = request.config.getoption("--resource-dir")
    if not path:
        return None
    return Path(path).resolve()


@pytest.fixture(scope="session")
def test_data_dir(request):
    path = os.path.join(os.path.dirname(__file__), "data")
    return Path(path).resolve()


@pytest_asyncio.fixture(scope="function")
async def client(
    request, executable: Path | None, resource_dir: Path | None, test_data_dir: Path
):
    config = request.config
    mode = config.getoption("--mode")

    cmd = [
        str(executable),
        f"--mode={mode}",
    ]

    if resource_dir:
        cmd.append(f"--resource-dir={resource_dir}")

    client = LSPClient(
        cmd,
        mode,
        config.getoption("--host"),
        config.getoption("--port"),
    )

    await client.start()
    yield client
    await client.exit()
