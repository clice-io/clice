import json
import asyncio
import logging
from typing import Any, Callable, Coroutine


class LSPError(Exception):
    pass


class LSPTransport:
    def __init__(self, commands: list[str], mode, host, port):
        self.commands = commands
        self.mode = mode
        self.host = host
        self.port = port
        self.logger = logging.getLogger(__name__)

        self.process: asyncio.subprocess.Process | None = None
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None

        self.request_id = 0
        self.pending_requests: dict[int, asyncio.Future] = {}
        self.notification_handlers: dict[
            str, Callable[[dict[str, Any] | None], Coroutine[Any, Any, None] | None]
        ] = {}
        self.message_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

        self._tasks: set[asyncio.Task] = set()
        self._stopping = False

    async def start(self):
        if self.mode == "pipe":
            self.logger.info(f"Starting LSP server via stdio: {self.commands}")
            self.process = await asyncio.create_subprocess_exec(
                *self.commands,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            self.reader = self.process.stdout
            self.writer = self.process.stdin
            self.logger.info(f"LSP server started with PID {self.process.pid}")
        elif self.mode == "socket":
            self.logger.info(
                f"Connecting to LSP server via socket: {self.host}:{self.port}"
            )
            self.reader, self.writer = await asyncio.open_connection(
                self.host, self.port
            )
            self.logger.info("Connected to LSP server via socket")
        else:
            raise ValueError("Invalid connection mode. Use 'pipe' or 'socket'")

        self._tasks.add(asyncio.create_task(self._read_messages()))
        self._tasks.add(asyncio.create_task(self._process_messages()))
        if self.process:
            assert self.process.stderr
            self._tasks.add(asyncio.create_task(self._read_stderr()))
            self._tasks.add(asyncio.create_task(self._monitor_process()))

    async def stop(self):
        if self._stopping:
            return
        self._stopping = True
        self.logger.info("Stopping LSPTransport")

        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)

        for future in self.pending_requests.values():
            future.set_exception(asyncio.CancelledError("LSPTransport is stopping"))
        self.pending_requests.clear()

        if self.writer and not self.writer.is_closing():
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except (BrokenPipeError, ConnectionResetError):
                pass

        if self.process and self.process.returncode is None:
            self.logger.info("Terminating LSP server process")
            try:
                self.process.terminate()
                await asyncio.wait_for(self.process.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                self.logger.warning("Process did not terminate gracefully, killing")
                self.process.kill()
                await self.process.wait()

        self.logger.info("LSPTransport stopped")

    async def _monitor_process(self):
        if not self.process:
            return
        return_code = await self.process.wait()
        self.logger.info(f"LSP server process exited with code {return_code}")
        if not self._stopping:
            asyncio.create_task(self.stop())

    async def _read_stderr(self):
        if not self.process or not self.process.stderr:
            return
        try:
            while not self.process.stderr.at_eof():
                line = await self.process.stderr.readline()
                if not line:
                    break
                self.logger.error(f"LSP Server STDERR: {line.decode().strip()}")
        except asyncio.CancelledError:
            pass
        except Exception as e:
            self.logger.error(f"Error reading stderr: {e}")

    async def _read_messages(self):
        try:
            while self.reader and not self.reader.at_eof():
                headers = {}
                while True:
                    header_line = await self.reader.readline()
                    if not header_line or header_line == b"\r\n":
                        break
                    key, value = header_line.decode("ascii").strip().split(":", 1)
                    headers[key.strip()] = value.strip()

                if not headers or "Content-Length" not in headers:
                    break

                content_length = int(headers["Content-Length"])
                body = await self.reader.readexactly(content_length)
                message = json.loads(body.decode("utf-8"))
                await self.message_queue.put(message)
        except (
            asyncio.IncompleteReadError,
            ConnectionResetError,
            BrokenPipeError,
        ):
            self.logger.info("Connection to LSP server lost")
        except asyncio.CancelledError:
            pass
        except Exception as e:
            if not self._stopping:
                self.logger.error(f"Unexpected error in message reader: {e}")
        finally:
            if not self._stopping:
                asyncio.create_task(self.stop())

    async def _handle_response(self, message: dict[str, Any]):
        request_id = message.get("id")
        if request_id is None:
            return

        future = self.pending_requests.pop(request_id, None)
        if not future or future.done():
            self.logger.warning(
                f"Received response for unknown or cancelled ID: {request_id}"
            )
            return

        if "result" in message:
            future.set_result(message["result"])
        elif "error" in message:
            future.set_exception(LSPError(message["error"]))
        else:
            future.set_exception(
                LSPError(f"LSP response missing 'result' or 'error': {message}")
            )

    async def _handle_notification(self, message: dict[str, Any]):
        method = message["method"]
        handler = self.notification_handlers.get(method)
        if not handler:
            self.logger.debug(f"Received unhandled notification: {method}")
            return
        try:
            params = message.get("params")
            result = handler(params)
            if asyncio.iscoroutine(result):
                await result
        except Exception as e:
            self.logger.error(f"Error in notification handler for {method}: {e}")

    async def _process_messages(self):
        try:
            while True:
                message = await self.message_queue.get()
                self.logger.debug(f"Received message: {message}")

                if "id" in message:
                    await self._handle_response(message)
                elif "method" in message:
                    await self._handle_notification(message)
                else:
                    self.logger.warning(f"Received malformed LSP message: {message}")
        except asyncio.CancelledError:
            pass
        except Exception as e:
            if not self._stopping:
                self.logger.error(f"Critical error processing message: {e}")
                asyncio.create_task(self.stop())

    async def _send_message(self, message: dict[str, Any]):
        if not self.writer or self.writer.is_closing():
            raise ConnectionError("LSP client writer is not available or closing")

        body = json.dumps(message, ensure_ascii=False).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")

        try:
            self.writer.write(header)
            self.writer.write(body)
            await self.writer.drain()
            self.logger.debug(f"Sent message: {message}")
        except (ConnectionResetError, BrokenPipeError) as e:
            self.logger.error(f"Error sending message: connection lost. {e}")
            if not self._stopping:
                asyncio.create_task(self.stop())
            raise

    async def send_request(
        self, method: str, params: dict[str, Any] | None = None
    ) -> Any:
        self.request_id += 1
        current_id = self.request_id
        message = {
            "jsonrpc": "2.0",
            "id": current_id,
            "method": method,
            "params": params if params is not None else {},
        }
        future = asyncio.get_running_loop().create_future()
        self.pending_requests[current_id] = future
        await self._send_message(message)
        return await future

    async def send_notification(
        self, method: str, params: dict[str, Any] | None = None
    ):
        message = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params if params is not None else {},
        }
        await self._send_message(message)

    def register_notification_handler(
        self,
        method: str,
        handler: Callable[[dict[str, Any] | None], Coroutine[Any, Any, None] | None],
    ):
        self.notification_handlers[method] = handler
