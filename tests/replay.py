#!/usr/bin/env python3
"""Replay recorded LSP traces against clice to detect hangs and crashes.

Usage:
    python replay.py tests/smoke/session.jsonl --clice build/clice
"""

import argparse
import asyncio
import json
import os
import re
import signal
import sys
import time

# Force line-buffered stdout so CI sees output immediately.
sys.stdout.reconfigure(line_buffering=True)
from pathlib import Path
from urllib.parse import quote, unquote

REPO_ROOT = Path(__file__).resolve().parent.parent

SERVER_REQUEST_DEFAULTS: dict[str, object] = {
    "window/workDoneProgress/create": None,
    "client/registerCapability": None,
    "workspace/configuration": [{}],
}


def load_trace(path: Path) -> list[dict]:
    """Load a JSONL trace file. Each line: {"ts": <ms>, "msg": "<json>"}"""
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def extract_original_workspace(records: list[dict]) -> str | None:
    for rec in records:
        parsed = json.loads(rec["msg"])
        if parsed.get("method") != "initialize":
            continue
        root_uri = parsed.get("params", {}).get("rootUri")
        if root_uri and root_uri.startswith("file://"):
            path = unquote(root_uri[len("file://") :])
            if len(path) > 2 and path[0] == "/" and path[2] == ":":
                path = path[1:]
            return path
    return None


def rewrite_workspace(original_ws: str) -> str | None:
    """Map recorded workspace path to the current repo location."""
    for marker in ("tests/data/", "tests/smoke/"):
        idx = original_ws.find(marker)
        if idx != -1:
            return str(REPO_ROOT / original_ws[idx:])
    return None


def rewrite_records(records: list[dict], original_ws: str, new_ws: str) -> list[dict]:
    """Text-level workspace path replacement in all messages.

    Backslashes in Windows paths must be doubled inside JSON strings.
    """
    new_ws_json = new_ws.replace("\\", "\\\\")
    original_encoded = quote(original_ws, safe="/:")
    new_encoded = quote(new_ws, safe="/:")
    new_encoded_json = new_encoded.replace("\\", "\\\\")
    rewritten = []
    for rec in records:
        msg = rec["msg"]
        msg = msg.replace(original_ws, new_ws_json)
        if original_encoded != original_ws:
            msg = msg.replace(original_encoded, new_encoded_json)
        rewritten.append({"ts": rec["ts"], "msg": msg})
    return rewritten


def print_trace_info(name: str, records: list[dict], workspace: str | None):
    methods: dict[str, int] = {}
    for rec in records:
        m = json.loads(rec["msg"]).get("method")
        if m:
            methods[m] = methods.get(m, 0) + 1
    print(f"--- {name} ---")
    print(f"  messages: {len(records)}")
    if workspace:
        print(f"  workspace: {workspace}")
    if records:
        print(f"  duration: {(records[-1]['ts'] - records[0]['ts']) / 1000.0:.1f}s")
    top = sorted(methods.items(), key=lambda x: -x[1])[:8]
    print(f"  methods: {', '.join(f'{m}({n})' for m, n in top)}")


async def read_lsp_message(reader: asyncio.StreamReader) -> dict | None:
    header = b""
    while True:
        line = await reader.readline()
        if not line:
            return None
        header += line
        if header.endswith(b"\r\n\r\n"):
            break
    match = re.search(rb"Content-Length:\s*(\d+)", header)
    if not match:
        return None
    return json.loads(await reader.readexactly(int(match.group(1))))


async def write_lsp_message(writer: asyncio.StreamWriter, payload: str):
    encoded = payload.encode("utf-8")
    writer.write(f"Content-Length: {len(encoded)}\r\n\r\n".encode("ascii") + encoded)
    await writer.drain()


async def replay_one(
    trace_path: Path, clice_bin: Path, timeout: int, wall_timeout: int = 300
) -> bool | None:
    """Replay a single trace. Returns True=PASS, False=FAIL, None=SKIP."""
    records = load_trace(trace_path)
    if not records:
        print(f"SKIP: {trace_path.name} (empty)")
        return None

    original_ws = extract_original_workspace(records)
    display_ws = original_ws
    if original_ws is not None:
        new_ws = rewrite_workspace(original_ws)
        if new_ws and original_ws != new_ws:
            records = rewrite_records(records, original_ws, new_ws)
            display_ws = new_ws

    print_trace_info(trace_path.name, records, display_ws)

    env = dict(os.environ)
    if sys.platform == "darwin":
        prev = env.get("ASAN_OPTIONS", "")
        env["ASAN_OPTIONS"] = f"{prev}:detect_leaks=0" if prev else "detect_leaks=0"

    proc = await asyncio.create_subprocess_exec(
        str(clice_bin),
        "--mode",
        "pipe",
        env=env,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    pending: dict[int | str, asyncio.Future] = {}

    async def reader_loop():
        try:
            while True:
                msg = await read_lsp_message(proc.stdout)
                if msg is None:
                    break
                msg_id, method = msg.get("id"), msg.get("method")
                if msg_id is not None and method is not None:
                    resp = SERVER_REQUEST_DEFAULTS.get(method)
                    await write_lsp_message(
                        proc.stdin,
                        json.dumps({"jsonrpc": "2.0", "id": msg_id, "result": resp}),
                    )
                elif msg_id is not None:
                    fut = pending.pop(msg_id, None)
                    if fut and not fut.done():
                        fut.set_result(msg)
                    if "error" in msg:
                        err = msg["error"]
                        print(
                            f"  ERROR response id={msg_id}: "
                            f"code={err.get('code')}, message={err.get('message')}"
                        )
        except (asyncio.IncompleteReadError, ConnectionError, BrokenPipeError):
            pass
        finally:
            for fut in pending.values():
                if not fut.done():
                    fut.cancel()

    wall_start = time.monotonic()
    reader_task = asyncio.create_task(reader_loop())
    success = True
    last_method = None
    sent_count = 0

    wall_deadline = wall_start + wall_timeout

    def remaining_wall():
        return max(0, wall_deadline - time.monotonic())

    try:
        for i, rec in enumerate(records):
            if remaining_wall() <= 0:
                elapsed = time.monotonic() - wall_start
                print(
                    f"  result: TIMEOUT (wall-clock {wall_timeout}s exceeded, {elapsed:.1f}s)"
                )
                success = False
                break

            if i > 0:
                delay = rec["ts"] - records[i - 1]["ts"]
                if delay > 0:
                    await asyncio.sleep(delay / 1000.0)

            parsed = json.loads(rec["msg"])
            method = parsed.get("method")
            msg_id = parsed.get("id")
            last_method = method or last_method

            # Before shutdown/exit, wait for all pending responses
            if method in ("shutdown", "exit") and pending:
                try:
                    await asyncio.wait_for(
                        asyncio.gather(*pending.values(), return_exceptions=True),
                        timeout=min(timeout, remaining_wall()),
                    )
                except asyncio.TimeoutError:
                    elapsed = time.monotonic() - wall_start
                    print(
                        f"  result: HANG ({len(pending)} pending before {method}, {elapsed:.1f}s)"
                    )
                    success = False
                    break
                pending.clear()

            if msg_id is not None and method is not None:
                pending[msg_id] = asyncio.get_event_loop().create_future()

            try:
                await asyncio.wait_for(
                    write_lsp_message(proc.stdin, rec["msg"]),
                    timeout=min(30, remaining_wall()),
                )
            except asyncio.TimeoutError:
                elapsed = time.monotonic() - wall_start
                print(
                    f"  result: HANG (write blocked at {last_method},"
                    f" sent={sent_count}/{len(records)}, {elapsed:.1f}s)"
                )
                success = False
                break
            sent_count = i + 1

    except (ConnectionError, BrokenPipeError):
        elapsed = time.monotonic() - wall_start
        try:
            await asyncio.wait_for(proc.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
        print(
            f"  result: CRASH (broken pipe at {last_method}, exit={proc.returncode},"
            f" sent={sent_count}/{len(records)}, {elapsed:.1f}s)"
        )
        success = False

    # Wait for remaining responses after all messages sent
    if success and pending:
        try:
            await asyncio.wait_for(
                asyncio.gather(*pending.values(), return_exceptions=True),
                timeout=min(timeout, remaining_wall()),
            )
        except asyncio.TimeoutError:
            elapsed = time.monotonic() - wall_start
            print(f"  result: HANG ({len(pending)} pending after {elapsed:.1f}s)")
            success = False

    if success:
        try:
            proc.stdin.close()
            await proc.stdin.wait_closed()
        except (ConnectionError, BrokenPipeError):
            pass

    try:
        await asyncio.wait_for(proc.wait(), timeout=10.0)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()

    reader_task.cancel()
    try:
        await reader_task
    except asyncio.CancelledError:
        pass

    stderr_data = await proc.stderr.read()
    elapsed = time.monotonic() - wall_start
    returncode = proc.returncode

    def print_stderr():
        if stderr_data:
            for line in stderr_data.decode("utf-8", errors="replace").splitlines()[
                -20:
            ]:
                print(f"  | {line}")

    if not success:
        print_stderr()
        return False

    if returncode and returncode != 0:
        sig = ""
        if returncode < 0:
            try:
                sig = f" ({signal.Signals(-returncode).name})"
            except (ValueError, AttributeError):
                pass
        print(f"  result: CRASH (exit={returncode}{sig}, {elapsed:.1f}s)")
        print_stderr()
        return False

    print(f"  result: PASS ({elapsed:.1f}s)")
    return True


async def async_main(args):
    passed = failed = skipped = 0
    for trace in args.traces:
        if not trace.exists():
            print(f"SKIP: {trace} (not found)")
            skipped += 1
            continue
        result = await replay_one(trace, args.clice, args.timeout, args.wall_timeout)
        if result is None:
            skipped += 1
        elif result:
            passed += 1
        else:
            failed += 1

    total = passed + failed + skipped
    print(f"\n{passed}/{total} passed", end="")
    if skipped:
        print(f", {skipped} skipped", end="")
    if failed:
        print(f", {failed} FAILED", end="")
    print()
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description="Replay LSP traces against clice")
    p.add_argument("traces", nargs="+", type=Path, help="JSONL trace files")
    p.add_argument("--clice", required=True, type=Path, help="Path to clice binary")
    p.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Per-request timeout in seconds (default: 120)",
    )
    p.add_argument(
        "--wall-timeout",
        type=int,
        default=300,
        help="Max wall-clock time per trace in seconds (default: 300)",
    )
    args = p.parse_args()
    sys.exit(asyncio.run(async_main(args)))


if __name__ == "__main__":
    main()
