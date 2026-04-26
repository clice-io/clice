"""Tests for the agentic TCP protocol."""

import pytest


@pytest.mark.workspace("hello_world")
async def test_compile_command(agentic, workspace):
    main_cpp = str(workspace / "main.cpp")
    resp = await agentic.request("agentic/compileCommand", {"path": main_cpp})
    result = resp["result"]
    assert result["file"] == main_cpp
    assert result["directory"] == str(workspace)
    assert len(result["arguments"]) > 0


@pytest.mark.workspace("hello_world")
async def test_compile_command_fallback(agentic, workspace):
    resp = await agentic.request(
        "agentic/compileCommand", {"path": "/nonexistent/file.cpp"}
    )
    result = resp["result"]
    assert result["file"] == "/nonexistent/file.cpp"


@pytest.mark.workspace("hello_world")
async def test_multiple_requests(agentic, workspace):
    main_cpp = str(workspace / "main.cpp")
    for _ in range(3):
        resp = await agentic.request("agentic/compileCommand", {"path": main_cpp})
        assert "result" in resp
        assert resp["result"]["file"] == main_cpp
