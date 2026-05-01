"""Tests for the agentic protocol handlers."""

import asyncio
import json
import socket
import subprocess
from concurrent.futures import ThreadPoolExecutor

import pytest

from tests.integration.utils.wait import wait_for_index


class AgenticRpcClient:
    """Minimal JSON-RPC client that speaks Content-Length framing over TCP."""

    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.request_id = 0
        self.buffer = b""

    def request(self, method: str, params: dict):
        self.request_id += 1
        body = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": self.request_id,
                "method": method,
                "params": params,
            }
        )
        payload = f"Content-Length: {len(body)}\r\n\r\n{body}".encode("utf-8")
        self.sock.sendall(payload)
        return self._read_response()

    def _read_response(self):
        while b"\r\n\r\n" not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("connection closed")
            self.buffer += data

        header_end = self.buffer.index(b"\r\n\r\n")
        headers = self.buffer[:header_end].decode("utf-8")
        self.buffer = self.buffer[header_end + 4 :]

        content_length = 0
        for line in headers.split("\r\n"):
            if line.lower().startswith("content-length:"):
                content_length = int(line.split(":")[1].strip())

        while len(self.buffer) < content_length:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("connection closed")
            self.buffer += data

        body = self.buffer[:content_length].decode("utf-8")
        self.buffer = self.buffer[content_length:]
        return json.loads(body)

    def close(self):
        self.sock.close()


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


# ── Existing CLI tests ──────────────────────────────────────────────


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
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        free_port = s.getsockname()[1]
    result = run_agentic(executable, "127.0.0.1", free_port, "/some/file.cpp")
    assert result.returncode != 0


@pytest.mark.workspace("hello_world")
async def test_concurrent_connections(agentic, workspace):
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


# ── Agentic protocol handler tests ─────────────────────────────────
# Wire format uses camelCase field names (lsp_config lower_camel rename).


@pytest.fixture
async def indexed_agentic(request, executable, workspace):
    """Start server with LSP+agentic, compile a file, wait for indexing."""
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

    rpc = AgenticRpcClient(host, port)

    for _ in range(30):
        resp = rpc.request("agentic/symbolSearch", {"query": "add"})
        if "result" in resp and resp["result"]["symbols"]:
            break
        await asyncio.sleep(1)

    yield rpc, workspace

    rpc.close()
    c.close(uri)
    await _shutdown_client(c)


@pytest.mark.workspace("index_features")
async def test_rpc_compile_command(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    path = (workspace / "main.cpp").as_posix()
    resp = rpc.request("agentic/compileCommand", {"path": path})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["file"] == path
    assert len(result["arguments"]) > 0


@pytest.mark.workspace("index_features")
async def test_rpc_project_files(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/projectFiles", {})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["total"] > 0
    paths = [f["path"] for f in result["files"]]
    assert any("main.cpp" in p for p in paths)


@pytest.mark.workspace("index_features")
async def test_rpc_project_files_filter(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/projectFiles", {"filter": "source"})
    assert "result" in resp
    for f in resp["result"]["files"]:
        assert f["kind"] == "source"


@pytest.mark.workspace("index_features")
async def test_rpc_symbol_search(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/symbolSearch", {"query": "add"})
    assert "result" in resp, f"unexpected response: {resp}"
    names = [s["name"] for s in resp["result"]["symbols"]]
    assert "add" in names


@pytest.mark.workspace("index_features")
async def test_rpc_symbol_search_kind(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/symbolSearch", {"query": "Animal", "kindFilter": ["Struct"]}
    )
    assert "result" in resp
    for s in resp["result"]["symbols"]:
        assert s["kind"] == "Struct"


@pytest.mark.workspace("index_features")
async def test_rpc_symbol_search_max(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/symbolSearch", {"query": "", "maxResults": 3})
    assert "result" in resp
    assert len(resp["result"]["symbols"]) <= 3


@pytest.mark.workspace("index_features")
async def test_rpc_read_symbol(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/readSymbol", {"name": "add"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["name"] == "add"
    assert result["symbolId"] != 0
    assert "int" in result["text"] or "add" in result["text"]


@pytest.mark.workspace("index_features")
async def test_rpc_read_symbol_by_id(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp1 = rpc.request("agentic/readSymbol", {"name": "add"})
    assert "result" in resp1
    sid = resp1["result"]["symbolId"]

    resp2 = rpc.request("agentic/readSymbol", {"symbolId": sid})
    assert "result" in resp2
    assert resp2["result"]["name"] == "add"
    assert resp2["result"]["symbolId"] == sid


@pytest.mark.workspace("index_features")
async def test_rpc_document_symbols(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    path = (workspace / "main.cpp").as_posix()
    resp = rpc.request("agentic/documentSymbols", {"path": path})
    assert "result" in resp, f"unexpected response: {resp}"
    names = [s["name"] for s in resp["result"]["symbols"]]
    assert any("add" in n for n in names), f"expected 'add' in {names}"
    assert any("main" in n for n in names), f"expected 'main' in {names}"


@pytest.mark.workspace("index_features")
async def test_rpc_definition(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/definition", {"name": "add"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["name"] == "add"
    assert result["definition"] is not None
    assert "main.cpp" in result["definition"]["file"]


@pytest.mark.workspace("index_features")
async def test_rpc_definition_by_position(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    path = (workspace / "main.cpp").as_posix()
    resp = rpc.request("agentic/definition", {"path": path, "line": 19})
    assert "result" in resp, f"unexpected response: {resp}"
    assert resp["result"]["name"] == "add"


@pytest.mark.workspace("index_features")
async def test_rpc_references(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/references", {"name": "global_var"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["name"] == "global_var"
    assert result["total"] >= 2


@pytest.mark.workspace("index_features")
async def test_rpc_references_include_decl(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/references", {"name": "global_var", "includeDeclaration": True}
    )
    assert "result" in resp
    assert resp["result"]["total"] >= 3


@pytest.mark.workspace("index_features")
async def test_rpc_call_graph_incoming(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/callGraph", {"name": "add", "direction": "callers"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["root"]["name"] == "add"
    caller_names = [c["name"] for c in result["callers"]]
    assert "compute" in caller_names, f"expected 'compute' in {caller_names}"


@pytest.mark.workspace("index_features")
async def test_rpc_call_graph_outgoing(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/callGraph", {"name": "compute", "direction": "callees"})
    assert "result" in resp, f"unexpected response: {resp}"
    callee_names = [c["name"] for c in resp["result"]["callees"]]
    assert "add" in callee_names, f"expected 'add' in {callee_names}"


@pytest.mark.workspace("index_features")
async def test_rpc_type_hierarchy_supertypes(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/typeHierarchy", {"name": "Dog", "direction": "supertypes"}
    )
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["root"]["name"] == "Dog"
    supertype_names = [t["name"] for t in result["supertypes"]]
    assert "Animal" in supertype_names, f"expected 'Animal' in {supertype_names}"


@pytest.mark.workspace("index_features")
async def test_rpc_type_hierarchy_subtypes(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/typeHierarchy", {"name": "Animal", "direction": "subtypes"}
    )
    assert "result" in resp, f"unexpected response: {resp}"
    subtype_names = [t["name"] for t in resp["result"]["subtypes"]]
    assert "Dog" in subtype_names, f"expected 'Dog' in {subtype_names}"
    assert "Cat" in subtype_names, f"expected 'Cat' in {subtype_names}"


@pytest.mark.workspace("index_features")
async def test_rpc_symbol_not_found(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/definition", {"name": "nonexistent_symbol_xyz"})
    assert "error" in resp


@pytest.mark.workspace("index_features")
async def test_rpc_symbol_id_roundtrip(indexed_agentic, workspace):
    """Search -> get symbolId -> definition -> verify consistency."""
    rpc, _ = indexed_agentic
    search = rpc.request("agentic/symbolSearch", {"query": "compute"})
    assert "result" in search
    symbols = search["result"]["symbols"]
    compute = next((s for s in symbols if s["name"] == "compute"), None)
    assert compute is not None, f"'compute' not found in {[s['name'] for s in symbols]}"

    defn = rpc.request("agentic/definition", {"symbolId": compute["symbolId"]})
    assert "result" in defn
    assert defn["result"]["name"] == "compute"
    assert defn["result"]["symbolId"] == compute["symbolId"]
