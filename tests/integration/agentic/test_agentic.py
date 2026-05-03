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
    else:
        pytest.fail("agentic/symbolSearch never returned indexed symbols")

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
    symbols = resp["result"]["symbols"]
    add_sym = next((s for s in symbols if s["name"] == "add"), None)
    assert add_sym is not None, f"'add' not found in {[s['name'] for s in symbols]}"
    assert add_sym["kind"] == "Function"
    assert add_sym["line"] == 19
    assert add_sym["symbolId"] != 0
    assert "main.cpp" in add_sym["file"]


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
    assert result["startLine"] == 19
    assert result["endLine"] == 21
    assert "int add(int a, int b)" in result["text"]
    assert "return a + b;" in result["text"]


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
    symbols = resp["result"]["symbols"]
    names = [s["name"] for s in symbols]
    kinds = [s["kind"] for s in symbols]
    assert "add" in names, f"expected 'add' in {names}"
    assert "main" in names, f"expected 'main' in {names}"
    assert "global_var" in names, f"expected 'global_var' in {names}"
    assert "Parameter" not in kinds, (
        f"Parameters should be filtered: {list(zip(names, kinds))}"
    )


@pytest.mark.workspace("index_features")
async def test_rpc_definition(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/definition", {"name": "add"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["name"] == "add"
    assert result["definition"] is not None
    defn = result["definition"]
    assert "main.cpp" in defn["file"]
    assert defn["startLine"] == 19
    assert defn["endLine"] == 21
    assert "int add(int a, int b)" in defn["text"]
    assert "return a + b;" in defn["text"]


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
    assert result["total"] == 2
    lines = sorted(r["line"] for r in result["references"])
    assert lines == [34, 38]
    contexts = [r["context"] for r in result["references"]]
    assert any("global_var + 1" in c for c in contexts)
    assert any("global_var * 2" in c for c in contexts)


@pytest.mark.workspace("index_features")
async def test_rpc_references_include_decl(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/references", {"name": "global_var", "includeDeclaration": True}
    )
    assert "result" in resp
    result = resp["result"]
    assert result["total"] == 3
    lines = sorted(r["line"] for r in result["references"])
    assert 31 in lines, f"expected declaration line 31 in {lines}"


@pytest.mark.workspace("index_features")
async def test_rpc_call_graph_incoming(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/callGraph", {"name": "add", "direction": "callers"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["root"]["name"] == "add"
    assert result["root"]["line"] == 19
    assert result["root"]["symbolId"] != 0
    callers = result["callers"]
    caller_names = [c["name"] for c in callers]
    assert "compute" in caller_names, f"expected 'compute' in {caller_names}"
    compute = next(c for c in callers if c["name"] == "compute")
    assert compute["line"] == 24
    assert compute["symbolId"] != 0
    assert result["callees"] == []


@pytest.mark.workspace("index_features")
async def test_rpc_call_graph_outgoing(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/callGraph", {"name": "compute", "direction": "callees"})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["root"]["name"] == "compute"
    callees = result["callees"]
    callee_names = [c["name"] for c in callees]
    assert "add" in callee_names, f"expected 'add' in {callee_names}"
    add_entry = next(c for c in callees if c["name"] == "add")
    assert add_entry["line"] == 19
    assert result["callers"] == []


@pytest.mark.workspace("index_features")
async def test_rpc_type_hierarchy_supertypes(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/typeHierarchy", {"name": "Dog", "direction": "supertypes"}
    )
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["root"]["name"] == "Dog"
    assert result["root"]["line"] == 9
    supertypes = result["supertypes"]
    supertype_names = [t["name"] for t in supertypes]
    assert "Animal" in supertype_names, f"expected 'Animal' in {supertype_names}"
    animal = next(t for t in supertypes if t["name"] == "Animal")
    assert animal["line"] == 2
    assert animal["symbolId"] != 0


@pytest.mark.workspace("index_features")
async def test_rpc_type_hierarchy_subtypes(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request(
        "agentic/typeHierarchy", {"name": "Animal", "direction": "subtypes"}
    )
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["root"]["name"] == "Animal"
    assert result["root"]["line"] == 2
    subtypes = result["subtypes"]
    subtype_names = [t["name"] for t in subtypes]
    assert "Dog" in subtype_names, f"expected 'Dog' in {subtype_names}"
    assert "Cat" in subtype_names, f"expected 'Cat' in {subtype_names}"
    dog = next(t for t in subtypes if t["name"] == "Dog")
    assert dog["line"] == 9
    cat = next(t for t in subtypes if t["name"] == "Cat")
    assert cat["line"] == 14


@pytest.mark.workspace("index_features")
async def test_rpc_status(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/status", {})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert isinstance(result["idle"], bool)
    assert result["total"] > 0
    assert isinstance(result["pending"], int)
    assert isinstance(result["indexed"], int)


@pytest.mark.workspace("hello_world")
async def test_rpc_shutdown(executable, workspace):
    """Shutdown notification should cause the server to exit."""
    from tests.integration.utils.client import CliceClient
    from tests.conftest import _shutdown_client, _find_free_port

    host = "127.0.0.1"
    port = _find_free_port()
    cmd = [str(executable), "--mode", "pipe", "--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)
    init_options = {"project": {"cache_dir": str(workspace / ".clice")}}
    await c.initialize(workspace, initialization_options=init_options)

    rpc = AgenticRpcClient(host, port)
    body = json.dumps({"jsonrpc": "2.0", "method": "agentic/shutdown", "params": {}})
    rpc.sock.sendall(f"Content-Length: {len(body)}\r\n\r\n{body}".encode())
    rpc.sock.settimeout(5)
    try:
        rpc.sock.recv(4096)
    except (socket.timeout, OSError):
        pass
    rpc.sock.close()

    import asyncio

    for _ in range(20):
        if c._server.returncode is not None:
            break
        await asyncio.sleep(0.5)
    assert c._server.returncode is not None, "Server did not exit after shutdown"


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


@pytest.mark.workspace("index_features")
async def test_rpc_file_deps(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    path = (workspace / "main.cpp").as_posix()
    resp = rpc.request("agentic/fileDeps", {"path": path})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["file"] == path
    assert isinstance(result["includes"], list)
    assert isinstance(result["includers"], list)


@pytest.mark.workspace("index_features")
async def test_rpc_file_deps_direction(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    path = (workspace / "main.cpp").as_posix()
    resp = rpc.request("agentic/fileDeps", {"path": path, "direction": "includes"})
    assert "result" in resp
    assert resp["result"]["includers"] == []


@pytest.mark.workspace("index_features")
async def test_rpc_file_deps_unknown(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/fileDeps", {"path": "/nonexistent/file.cpp"})
    assert "result" in resp
    assert resp["result"]["includes"] == []
    assert resp["result"]["includers"] == []


@pytest.mark.workspace("index_features")
async def test_rpc_impact_analysis(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    path = (workspace / "main.cpp").as_posix()
    resp = rpc.request("agentic/impactAnalysis", {"path": path})
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert isinstance(result["directDependents"], list)
    assert isinstance(result["transitiveDependents"], list)
    assert isinstance(result["affectedModules"], list)


@pytest.mark.workspace("index_features")
async def test_rpc_impact_analysis_unknown(indexed_agentic, workspace):
    rpc, _ = indexed_agentic
    resp = rpc.request("agentic/impactAnalysis", {"path": "/nonexistent/file.cpp"})
    assert "result" in resp
    assert resp["result"]["directDependents"] == []


async def test_shutdown_during_indexing(executable, tmp_path):
    """Shutdown during active background indexing must exit cleanly."""
    from tests.integration.utils.client import CliceClient
    from tests.conftest import _find_free_port

    workspace = tmp_path / "ws"
    workspace.mkdir()

    entries = []
    for i in range(20):
        src = workspace / f"file_{i}.cpp"
        src.write_text(
            f"struct Type_{i} {{ int v = {i}; void m() {{}} }};\n"
            f"int func_{i}(int x) {{ return x + {i}; }}\n"
            f"int caller_{i}() {{ return func_{i}({i}); }}\n"
        )
        entries.append(
            {
                "directory": workspace.as_posix(),
                "file": src.as_posix(),
                "arguments": ["clang++", "-std=c++17", "-fsyntax-only", src.as_posix()],
            }
        )

    (workspace / "compile_commands.json").write_text(json.dumps(entries))

    host = "127.0.0.1"
    port = _find_free_port()
    cmd = [str(executable), "--mode", "pipe", "--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    init_options = {
        "project": {
            "cache_dir": str(workspace / ".clice"),
            "idle_timeout_ms": 0,
        }
    }
    await c.initialize(workspace, initialization_options=init_options)

    # Give indexing a moment to start, then send shutdown
    await asyncio.sleep(0.5)

    rpc = AgenticRpcClient(host, port)
    body = json.dumps({"jsonrpc": "2.0", "method": "agentic/shutdown", "params": {}})
    rpc.sock.sendall(f"Content-Length: {len(body)}\r\n\r\n{body}".encode())
    rpc.sock.settimeout(5)
    try:
        rpc.sock.recv(4096)
    except (socket.timeout, OSError):
        pass
    rpc.sock.close()

    for _ in range(30):
        if c._server.returncode is not None:
            break
        await asyncio.sleep(0.5)

    assert c._server.returncode is not None, "Server did not exit after shutdown"
    assert c._server.returncode >= 0, (
        f"Server crashed with signal {-c._server.returncode}"
    )
