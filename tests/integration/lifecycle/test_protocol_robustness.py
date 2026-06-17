"""Initialize must tolerate unknown enum values and fields from newer clients."""

import asyncio
import json

import pytest
from lsprotocol.types import InitializeParams

from tests.integration.utils.injection import build_params
from tests.replay import SERVER_REQUEST_DEFAULTS, read_lsp_message, write_lsp_message

INJECTION_FLOOR = 5


async def request(proc, msg_id: int, method: str, params) -> dict:
    payload = {"jsonrpc": "2.0", "id": msg_id, "method": method, "params": params}
    await write_lsp_message(proc.stdin, json.dumps(payload))
    while True:
        msg = await read_lsp_message(proc.stdout)
        assert msg is not None, "server closed the stream"
        if msg.get("id") == msg_id and "method" not in msg:
            return msg
        # Answer server-initiated requests so the handshake can progress.
        if msg.get("id") is not None and msg.get("method") is not None:
            reply = {
                "jsonrpc": "2.0",
                "id": msg["id"],
                "result": SERVER_REQUEST_DEFAULTS.get(msg["method"]),
            }
            await write_lsp_message(proc.stdin, json.dumps(reply))


async def notify(proc, method: str, params) -> None:
    payload = {"jsonrpc": "2.0", "method": method, "params": params}
    await write_lsp_message(proc.stdin, json.dumps(payload))


@pytest.mark.workspace("hello_world")
@pytest.mark.parametrize(
    "mode", ["unknown_string_enums", "out_of_range_int_enums", "unknown_fields"]
)
async def test_initialize_hostile_params(executable, workspace, mode):
    params, stats = build_params(InitializeParams, mode)
    injected = {
        "unknown_string_enums": stats.string_enums,
        "out_of_range_int_enums": stats.int_enums,
        "unknown_fields": stats.unknown_fields,
    }[mode]
    assert injected >= INJECTION_FLOOR, f"builder injected too little: {injected}"

    params["processId"] = 12345
    params["rootPath"] = str(workspace)
    params["rootUri"] = workspace.as_uri()
    params["workspaceFolders"] = [{"uri": workspace.as_uri(), "name": "test"}]
    params["initializationOptions"] = {
        "project": {"cache_dir": str(workspace / ".clice")},
    }

    proc = await asyncio.create_subprocess_exec(
        str(executable),
        "server",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    try:
        response = await asyncio.wait_for(request(proc, 0, "initialize", params), 30)
        assert "error" not in response, response.get("error")
        assert response["result"]["serverInfo"]["name"] == "clice"
        assert "capabilities" in response["result"]

        await notify(proc, "initialized", {})
        shutdown = await asyncio.wait_for(request(proc, 1, "shutdown", None), 30)
        assert "error" not in shutdown
        await notify(proc, "exit", None)
        await asyncio.wait_for(proc.wait(), 30)
        assert proc.returncode == 0
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
