"""Lightweight async JSON-RPC client for the agentic TCP protocol."""

import asyncio
import json
import re


class AgenticClient:
    """Connects to the clice agentic TCP endpoint and sends JSON-RPC requests."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self._reader = reader
        self._writer = writer
        self._next_id = 1

    @classmethod
    async def connect(cls, host: str, port: int, *, timeout: float = 10.0):
        deadline = asyncio.get_event_loop().time() + timeout
        while True:
            try:
                reader, writer = await asyncio.open_connection(host, port)
                return cls(reader, writer)
            except (ConnectionRefusedError, OSError):
                if asyncio.get_event_loop().time() >= deadline:
                    raise
                await asyncio.sleep(0.1)

    async def request(
        self, method: str, params: dict, *, timeout: float = 30.0
    ) -> dict:
        msg_id = self._next_id
        self._next_id += 1
        payload = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": msg_id,
                "method": method,
                "params": params,
            }
        )
        encoded = payload.encode("utf-8")
        self._writer.write(
            f"Content-Length: {len(encoded)}\r\n\r\n".encode("ascii") + encoded
        )
        await self._writer.drain()
        response = await asyncio.wait_for(self._read_message(), timeout=timeout)
        assert response is not None, "connection closed before response"
        assert response.get("id") == msg_id
        return response

    async def _read_message(self) -> dict | None:
        header = b""
        while True:
            line = await self._reader.readline()
            if not line:
                return None
            header += line
            if header.endswith(b"\r\n\r\n"):
                break
        match = re.search(rb"Content-Length:\s*(\d+)", header)
        if not match:
            return None
        return json.loads(await self._reader.readexactly(int(match.group(1))))

    async def close(self):
        try:
            self._writer.close()
            await self._writer.wait_closed()
        except (ConnectionError, OSError):
            pass
