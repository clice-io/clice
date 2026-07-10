"""Server lifecycle helpers: spawn, graceful shutdown, clean-exit gates."""

import asyncio
import os
import socket
from pathlib import Path

import pytest

from tests.tools.checks import assert_no_anomaly
from tests.tools.client import CliceClient

SANITIZER_MARKERS = (
    "AddressSanitizer",
    "LeakSanitizer",
    "MemorySanitizer",
    "ThreadSanitizer",
    "UndefinedBehaviorSanitizer",
    "==ERROR:",
    "runtime error:",
)

next_port_offset = 0


def find_free_port() -> int:
    """Pick a port from a per-xdist-worker range.

    bind(0) draws from the kernel's shared pool: two concurrent xdist
    workers can grab the same port in the close-then-rebind gap. Disjoint
    per-worker ranges (below the ephemeral range) remove that race; the
    advancing offset avoids immediately reusing a just-released port.
    """
    global next_port_offset
    worker = os.environ.get("PYTEST_XDIST_WORKER", "gw0")
    suffix = worker.removeprefix("gw")
    index = int(suffix) if suffix.isdigit() else 0
    base = 21000 + index * 100
    for _ in range(100):
        port = base + next_port_offset
        next_port_offset = (next_port_offset + 1) % 100
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError(f"no free port in range {base}-{base + 99}")


async def make_client(executable: Path, workspace: Path) -> CliceClient:
    """Spawn a fresh clice server and initialize it. For multi-session tests."""
    c = CliceClient()
    await c.start_io(str(executable), "serve")
    await c.initialize(workspace)
    return c


def check_no_anomaly(request: pytest.FixtureRequest, c: CliceClient) -> None:
    """Teardown gate: anomalies are internal clice bugs — every test session
    must end without one. Tests that intentionally trigger anomalies opt out
    with @pytest.mark.allow_anomaly and assert on them explicitly."""
    if request.node.get_closest_marker("allow_anomaly") is not None:
        return
    assert_no_anomaly(c, c.workspace)


def server_stderr_excerpt(stderr_text: str) -> str:
    interesting = [
        line
        for line in stderr_text.splitlines()
        if "[warn]" in line
        or "[error]" in line
        or "Sanitizer" in line
        or "==ERROR:" in line
        or "runtime error:" in line
    ]
    return "\n".join(interesting[-80:])


async def assert_server_exited_cleanly(server, timeout: float = 10.0) -> None:
    failures: list[str] = []

    if server is None:
        return

    if server.returncode is None:
        try:
            await asyncio.wait_for(server.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            server.kill()
            await server.wait()
            failures.append(f"server did not exit within {timeout:g}s after shutdown")

    print(f"[server] exit code: {server.returncode}", flush=True)

    stderr_text = ""
    if server.stderr:
        try:
            stderr_data = await asyncio.wait_for(server.stderr.read(), timeout=2.0)
            stderr_text = stderr_data.decode("utf-8", errors="replace")
        except Exception as exc:
            failures.append(f"failed to collect server stderr: {exc!r}")

    for line in server_stderr_excerpt(stderr_text).splitlines():
        print(f"[server] {line}", flush=True)

    if server.returncode != 0:
        failures.append(f"server exited with code {server.returncode}")

    if any(marker in stderr_text for marker in SANITIZER_MARKERS):
        failures.append("server stderr contains sanitizer/runtime error output")

    if failures:
        excerpt = server_stderr_excerpt(stderr_text)
        if excerpt:
            failures.append("server stderr excerpt:\n" + excerpt)
        pytest.fail("\n".join(failures))


async def shutdown_client(c: CliceClient, *, verbose: bool = False) -> None:
    """Gracefully shut down a client, force-kill if needed."""
    try:
        await asyncio.wait_for(c.shutdown_async(None), timeout=10.0)
    except Exception:
        pass

    try:
        c.exit(None)
    except Exception:
        pass

    if verbose and c.log_messages:
        for msg in c.log_messages:
            level = {1: "ERROR", 2: "WARN", 3: "INFO", 4: "LOG"}.get(msg.type, "?")
            print(f"[logMessage/{level}] {msg.message}", flush=True)

    try:
        await assert_server_exited_cleanly(c.server)
    finally:
        try:
            await c.stop_io()
            await asyncio.sleep(0.1)
        except Exception:
            pass
