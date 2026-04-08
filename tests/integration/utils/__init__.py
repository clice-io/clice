"""Shared utilities for clice integration tests."""

from tests.integration.utils.client import CliceClient
from tests.integration.utils.workspace import doc, write_cdb, write_source
from tests.integration.utils.assertions import (
    assert_no_errors,
    assert_has_errors,
    assert_diagnostics_count,
)
from tests.integration.utils.wait import wait_for_recompile, wait_for_index
from tests.integration.utils.cache import list_pch_files, list_pcm_files, read_cache_json

__all__ = [
    "CliceClient",
    "doc",
    "write_cdb",
    "write_source",
    "assert_no_errors",
    "assert_has_errors",
    "assert_diagnostics_count",
    "wait_for_recompile",
    "wait_for_index",
    "list_pch_files",
    "list_pcm_files",
    "read_cache_json",
]
