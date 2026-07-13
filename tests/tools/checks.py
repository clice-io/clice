"""Observation and assertion helpers: diagnostics, anomalies, waits."""

import asyncio
import re
from pathlib import Path

from lsprotocol.types import (
    Diagnostic,
    DiagnosticSeverity,
    HoverParams,
    Position,
    TextDocumentIdentifier,
    WorkspaceSymbolParams,
)

# Standard timing constants — use these instead of hardcoded sleep values.
MTIME_GRANULARITY = 1.1  # Filesystem mtime precision (1s on some FSes, +0.1 margin)
SETTLE_TIME = 0.5  # Time for the server to stabilize after an operation
IDLE_TIMEOUT = 5.0  # Idle soak time in lifecycle tests


def locations_of(result):
    """Normalize a definition/references response to a list of Locations."""
    if result is None:
        return []
    if isinstance(result, (list, tuple)):
        return list(result)
    return [result]


async def wait_for_recompile(client, uri: str, *, timeout: float = 60.0) -> None:
    """Trigger recompilation via hover and wait for fresh diagnostics.

    Useful after didChange or on-disk file modifications. Sends a hover
    request at (0,0) to trigger ensure_compiled(), then waits for the
    resulting diagnostics notification.
    """
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )
    await asyncio.wait_for(event.wait(), timeout=timeout)


async def wait_for_index(
    client,
    uri: str,
    symbol_name: str = "add",
    *,
    timeout: int = 30,
) -> bool:
    """Poll workspace/symbol until a specific symbol appears in the index.

    Sends a hover to trigger compilation/indexing, then polls every second.
    Returns True if the symbol was found, False on timeout.
    """
    await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )

    for _ in range(timeout):
        result = await client.workspace_symbol_async(
            WorkspaceSymbolParams(query=symbol_name)
        )
        if result and any(s.name == symbol_name for s in result):
            return True
        await asyncio.sleep(1)
    return False


async def reference_uris(client, uri: str, line: int, character: int) -> list[str]:
    """URIs of the references at a position (declaration excluded)."""
    refs = await client.references_at(uri, line, character, include_declaration=False)
    return [ref.uri for ref in (refs or [])]


async def wait_for_reference(
    client, uri: str, line: int, character: int, expected_uri: str, timeout: int = 30
) -> bool:
    """Poll references at a position until expected_uri shows up."""
    for _ in range(timeout):
        if expected_uri in await reference_uris(client, uri, line, character):
            return True
        await asyncio.sleep(1)
    return False


ANOMALY_PATTERN = re.compile(r"\[anomaly:([A-Za-z]+)\]")


def anomalies_in_log_messages(client) -> list[str]:
    """Anomaly IDs from window/logMessage notifications (master process)."""
    found = []
    for msg in client.log_messages:
        match = ANOMALY_PATTERN.search(msg.message)
        if match:
            found.append(match.group(1))
    return found


def anomalies_in_log_files(workspace: Path | None) -> list[str]:
    """Anomaly IDs from master/worker log files under <workspace>/.clice/logs.

    Workers don't forward logMessage in v1.0, so their anomalies are only
    visible in their log files.
    """
    if workspace is None:
        return []
    logs_dir = Path(workspace) / ".clice" / "logs"
    if not logs_dir.exists():
        return []
    found = []
    for log_file in sorted(logs_dir.rglob("*.log")):
        for line in log_file.read_text(errors="replace").splitlines():
            match = ANOMALY_PATTERN.search(line)
            if match:
                found.append(f"{match.group(1)} ({log_file.name}: {line.strip()})")
    return found


CRASH_TRACE_MARKER = "=== CRASH STACK TRACE ==="


def crash_traces_in_log_files(workspace: Path | None) -> list[str]:
    """Crash stack traces recorded by crashed processes in their log files."""
    if workspace is None:
        return []
    logs_dir = Path(workspace) / ".clice" / "logs"
    if not logs_dir.exists():
        return []
    traces = []
    for log_file in sorted(logs_dir.rglob("*.log")):
        text = log_file.read_text(errors="replace")
        pos = text.find(CRASH_TRACE_MARKER)
        if pos != -1:
            traces.append(f"--- {log_file.name} ---\n{text[pos:]}")
    return traces


def assert_no_anomaly(client, workspace: Path | None = None) -> None:
    """Assert the session produced zero anomalies (client messages + logs).

    Runs in every integration test teardown: anomalies are internal clice
    bugs and must never occur on regular paths.
    """
    found = anomalies_in_log_messages(client)
    found += anomalies_in_log_files(workspace)
    # A crashed worker leaves its stack trace in its own log file; surface
    # it here so a one-off CI crash is diagnosable from the test output.
    traces = crash_traces_in_log_files(workspace) if found else []
    detail = "\n" + "\n".join(traces) if traces else ""
    assert not found, f"clice reported internal anomalies: {found}{detail}"


def guidance_messages(client) -> list[str]:
    """Guidance texts from window/logMessage notifications."""
    return [msg.message for msg in client.log_messages if "[guidance]" in msg.message]


def get_errors(diagnostics: list[Diagnostic]) -> list[Diagnostic]:
    """Filter diagnostics to errors only (severity == 1)."""
    return [d for d in diagnostics if d.severity == DiagnosticSeverity.Error]


def assert_no_errors(client, uri: str, msg: str = "") -> None:
    """Assert that there are no error-level diagnostics for the given URI."""
    diags = client.diagnostics.get(uri, [])
    errors = get_errors(diags)
    if msg:
        assert len(errors) == 0, f"{msg}: {errors}"
    else:
        assert len(errors) == 0, f"Expected no errors, got: {errors}"


def assert_has_errors(client, uri: str, msg: str = "") -> None:
    """Assert that there is at least one error-level diagnostic for the given URI."""
    diags = client.diagnostics.get(uri, [])
    errors = get_errors(diags)
    if msg:
        assert len(errors) > 0, msg
    else:
        assert len(errors) > 0, "Expected at least one error diagnostic"


def assert_diagnostics_count(
    client,
    uri: str,
    count: int,
    *,
    severity: int | None = None,
) -> None:
    """Assert exact number of diagnostics, optionally filtered by severity."""
    diags = client.diagnostics.get(uri, [])
    if severity is not None:
        diags = [d for d in diags if d.severity == severity]
    assert len(diags) == count, (
        f"Expected {count} diagnostics (severity={severity}), got {len(diags)}: {diags}"
    )


def assert_clean_compile(client, uri: str) -> None:
    """Assert the file compiled without any diagnostics at all."""
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected clean compile, got: {diags}"
