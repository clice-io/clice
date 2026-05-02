"""CLI-based tests for agentic mode — run clice --mode agentic as a subprocess."""

import json
import subprocess

import pytest

from tests.integration.utils.wait import wait_for_index


def run_cli(executable, host, port, method, **kwargs):
    cmd = [
        str(executable),
        "--mode",
        "agentic",
        "--host",
        host,
        "--port",
        str(port),
        "--method",
        method,
    ]
    for k, v in kwargs.items():
        cmd.extend([f"--{k}", str(v)])
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    return result


@pytest.fixture
async def indexed_server(request, executable, workspace):
    """Start server with LSP+agentic, compile a file, wait for indexing."""
    import asyncio
    from tests.integration.utils.client import CliceClient
    from tests.conftest import _shutdown_client, _find_free_port

    host = "127.0.0.1"
    port = _find_free_port()
    cmd = [str(executable), "--mode", "pipe", "--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    init_options = {"project": {"cache_dir": str(workspace / ".clice")}}
    await c.initialize(workspace, initialization_options=init_options)

    uri, _ = await c.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(c, uri, "add"), "Index not ready"

    from tests.integration.agentic.test_agentic import AgenticRpcClient

    rpc = AgenticRpcClient(host, port)
    for _ in range(30):
        resp = rpc.request("agentic/symbolSearch", {"query": "add"})
        if "result" in resp and resp["result"]["symbols"]:
            break
        await asyncio.sleep(1)
    rpc.close()

    yield executable, host, port, workspace

    c.close(uri)
    await _shutdown_client(c)


@pytest.mark.workspace("index_features")
async def test_cli_compile_command(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    path = (workspace / "main.cpp").as_posix()
    r = run_cli(exe, host, port, "compileCommand", path=path)
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["file"] == path
    assert len(data["arguments"]) > 0


@pytest.mark.workspace("index_features")
async def test_cli_symbol_search(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "symbolSearch", query="add")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    names = [s["name"] for s in data["symbols"]]
    assert "add" in names
    add_sym = next(s for s in data["symbols"] if s["name"] == "add")
    assert add_sym["kind"] == "Function"
    assert add_sym["line"] == 19


@pytest.mark.workspace("index_features")
async def test_cli_definition(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "definition", name="add")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["name"] == "add"
    defn = data["definition"]
    assert defn["startLine"] == 19
    assert defn["endLine"] == 21
    assert "return a + b;" in defn["text"]


@pytest.mark.workspace("index_features")
async def test_cli_definition_by_position(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    path = (workspace / "main.cpp").as_posix()
    r = run_cli(exe, host, port, "definition", path=path, line=19)
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["name"] == "add"


@pytest.mark.workspace("index_features")
async def test_cli_references(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "references", name="global_var")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["name"] == "global_var"
    assert data["total"] == 2
    lines = sorted(ref["line"] for ref in data["references"])
    assert lines == [34, 38]


@pytest.mark.workspace("index_features")
async def test_cli_read_symbol(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "readSymbol", name="compute")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["name"] == "compute"
    assert "add(1, 2)" in data["text"]


@pytest.mark.workspace("index_features")
async def test_cli_document_symbols(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    path = (workspace / "main.cpp").as_posix()
    r = run_cli(exe, host, port, "documentSymbols", path=path)
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    names = [s["name"] for s in data["symbols"]]
    assert "add" in names
    assert "main" in names
    assert "global_var" in names


@pytest.mark.workspace("index_features")
async def test_cli_call_graph(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "callGraph", name="add", direction="callers")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["root"]["name"] == "add"
    caller_names = [c["name"] for c in data["callers"]]
    assert "compute" in caller_names


@pytest.mark.workspace("index_features")
async def test_cli_type_hierarchy(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "typeHierarchy", name="Dog", direction="supertypes")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["root"]["name"] == "Dog"
    supertype_names = [t["name"] for t in data["supertypes"]]
    assert "Animal" in supertype_names


@pytest.mark.workspace("index_features")
async def test_cli_project_files(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "projectFiles")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert data["total"] > 0
    paths = [f["path"] for f in data["files"]]
    assert any("main.cpp" in p for p in paths)


@pytest.mark.workspace("index_features")
async def test_cli_status(indexed_server, workspace):
    exe, host, port, _ = indexed_server
    r = run_cli(exe, host, port, "status")
    assert r.returncode == 0, f"stderr: {r.stderr}"
    data = json.loads(r.stdout)
    assert isinstance(data["idle"], bool)
    assert data["total"] > 0
    assert isinstance(data["pending"], int)
    assert isinstance(data["indexed"], int)
