"""Diagnostic and anomaly assertion helpers for integration tests."""

import re
from pathlib import Path

from lsprotocol.types import Diagnostic, DiagnosticSeverity

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


def assert_no_anomaly(client, workspace: Path | None = None) -> None:
    """Assert the session produced zero anomalies (client messages + logs).

    Runs in every integration test teardown: anomalies are internal clice
    bugs and must never occur on regular paths.
    """
    found = anomalies_in_log_messages(client)
    found += anomalies_in_log_files(workspace)
    assert not found, f"clice reported internal anomalies: {found}"


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
