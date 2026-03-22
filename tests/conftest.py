import json
import os
import sys
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


@pytest.fixture(scope="session")
def executable(request) -> Path | None:
    executable = request.config.getoption("--executable")
    if not executable:
        return None

    path = Path(executable)
    if sys.platform.startswith("win") and path.suffix.lower() != ".exe":
        path_exe = path.with_name(path.name + ".exe")
        if path_exe.exists() or not path.exists():
            path = path_exe

    if not path.exists():
        pytest.exit(
            f"Error: 'clice' executable not found at '{executable}'. "
            "Please ensure the path is correct and the file exists.",
            returncode=64,
        )

    return path.resolve()


@pytest.fixture(scope="session")
def test_data_dir(request):
    path = os.path.join(os.path.dirname(__file__), "data")
    data_dir = Path(path).resolve()

    # Generate compile_commands.json for hello_world so the server actually compiles
    hw_dir = data_dir / "hello_world"
    main_cpp = hw_dir / "main.cpp"
    cdb_path = hw_dir / "compile_commands.json"
    if main_cpp.exists() and not cdb_path.exists():
        cdb = [
            {
                "directory": str(hw_dir),
                "file": str(main_cpp),
                "arguments": ["clang++", "-std=c++17", "-fsyntax-only", str(main_cpp)],
            }
        ]
        cdb_path.write_text(json.dumps(cdb, indent=2))

    return data_dir


@pytest_asyncio.fixture(scope="function")
async def client(request, executable: Path | None, test_data_dir: Path):
    config = request.config
    mode = config.getoption("--mode")

    host = config.getoption("--host")
    port = config.getoption("--port")

    cmd = [
        str(executable),
        "--mode",
        mode,
    ]

    if mode == "socket":
        cmd += ["--host", host, "--port", str(port)]

    client = LSPClient(
        cmd,
        mode,
        host,
        port,
    )

    await client.start()
    yield client
    await client.exit()
