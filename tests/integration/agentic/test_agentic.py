"""Tests for the agentic CLI client."""

import json
import socket
import subprocess
from concurrent.futures import ThreadPoolExecutor

import pytest


def run_agentic(executable, host, port, path, timeout=10):
    result = subprocess.run(
        [
            str(executable),
            "--mode",
            "agentic",
            "--host",
            host,
            "--port",
            str(port),
            "--path",
            path,
        ],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return result


@pytest.mark.workspace("hello_world")
async def test_compile_command(agentic, workspace):
    executable, host, port = agentic
    main_cpp = (workspace / "main.cpp").as_posix()
    result = run_agentic(executable, host, port, main_cpp)
    assert result.returncode == 0, f"stderr: {result.stderr}"
    data = json.loads(result.stdout)
    assert data["file"] == main_cpp
    assert data["directory"] == workspace.as_posix()
    assert len(data["arguments"]) > 0


@pytest.mark.workspace("hello_world")
async def test_compile_command_fallback(agentic, workspace):
    executable, host, port = agentic
    result = run_agentic(executable, host, port, "/nonexistent/file.cpp")
    assert result.returncode == 0, f"stderr: {result.stderr}"
    data = json.loads(result.stdout)
    assert data["file"] == "/nonexistent/file.cpp"


@pytest.mark.workspace("hello_world")
async def test_multiple_requests(agentic, workspace):
    executable, host, port = agentic
    main_cpp = (workspace / "main.cpp").as_posix()
    for _ in range(3):
        result = run_agentic(executable, host, port, main_cpp)
        assert result.returncode == 0, f"stderr: {result.stderr}"
        data = json.loads(result.stdout)
        assert data["file"] == main_cpp


async def test_connection_refused(executable):
    """Connecting to a port with no server should fail with non-zero exit."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        free_port = s.getsockname()[1]
    result = run_agentic(executable, "127.0.0.1", free_port, "/some/file.cpp")
    assert result.returncode != 0


@pytest.mark.workspace("hello_world")
async def test_concurrent_connections(agentic, workspace):
    """Multiple agentic clients connecting simultaneously should all succeed."""
    executable, host, port = agentic
    main_cpp = (workspace / "main.cpp").as_posix()

    def do_request(_):
        return run_agentic(executable, host, port, main_cpp)

    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(do_request, range(4)))

    for r in results:
        assert r.returncode == 0, f"stderr: {r.stderr}"
        data = json.loads(r.stdout)
        assert data["file"] == main_cpp
