"""Diagnostic assertion helpers for integration tests."""

from lsprotocol.types import Diagnostic, DiagnosticSeverity


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
    client, uri: str, count: int, *, severity: int | None = None,
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
