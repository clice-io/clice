"""Workspace and file utilities for integration tests."""

import json
from pathlib import Path

from lsprotocol.types import (
    DidChangeTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


def doc(uri: str) -> TextDocumentIdentifier:
    """Create a TextDocumentIdentifier from a URI string."""
    return TextDocumentIdentifier(uri=uri)


def write_cdb(
    workspace: Path,
    files: list[str],
    *,
    extra_args: list[str] | None = None,
    std: str = "c++17",
) -> None:
    """Write a compile_commands.json for the given source files.

    Args:
        workspace: Root directory of the workspace.
        files: List of source file names (relative to workspace).
        extra_args: Additional compiler arguments (e.g. ["-DFOO", "-I/bar"]).
        std: C++ standard version (default: c++17).
    """
    entries = []
    for f in files:
        args = ["clang++", f"-std={std}", "-fsyntax-only"]
        if extra_args:
            args.extend(extra_args)
        args.append(str(workspace / f))
        entries.append(
            {
                "directory": str(workspace),
                "file": str(workspace / f),
                "arguments": args,
            }
        )
    (workspace / "compile_commands.json").write_text(json.dumps(entries, indent=2))


def write_source(workspace: Path, name: str, content: str) -> Path:
    """Write a source file to the workspace directory. Returns the file path."""
    path = workspace / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


def did_change(client, uri: str, version: int, text: str) -> None:
    """Send a didChange notification with whole-document replacement."""
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=version),
            content_changes=[TextDocumentContentChangeWholeDocument(text=text)],
        )
    )
