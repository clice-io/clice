"""Tests for the agentic CLI client."""

import json
import subprocess

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
