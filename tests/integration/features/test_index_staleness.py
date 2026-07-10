"""Touching a header (mtime bump, identical content) must not reindex its
closed dependents — the content-hash staleness check is the storm filter."""

import asyncio

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.compile_commands import write_cdb
from tests.tools.workspace import cache_root, pin_cache_to_workspace
from tests.tools.checks import MTIME_GRANULARITY

HEADER = "#pragma once\ninline int alpha() { return 1; }\n"
CLOSED_TU = '#include "header.h"\nint use() { return alpha(); }\n'


def shard_mtimes(workspace):
    """mtimes of the per-TU shards (numeric names; excludes the project blob)."""
    d = cache_root(workspace) / "index"
    shards = (p for p in d.glob("*.idx") if p.stem != "project") if d.exists() else ()
    return {p.name: p.stat().st_mtime_ns for p in shards}


def project_mtime(workspace):
    p = cache_root(workspace) / "index" / "project.idx"
    return p.stat().st_mtime_ns if p.exists() else 0


async def poll(predicate, timeout=30):
    for _ in range(timeout):
        if predicate():
            return True
        await asyncio.sleep(1)
    return False


async def test_touch_header_no_reindex(executable, tmp_path):
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text(HEADER)
    (tmp_path / "closed.cpp").write_text(CLOSED_TU)
    write_cdb(tmp_path, ["closed.cpp"])

    # Session 1: background-index the closed TU into a shard.
    c1 = await make_client(executable, tmp_path)
    assert await poll(lambda: shard_mtimes(tmp_path)), "closed TU never indexed"
    await shutdown_client(c1)

    # Touch the header: bump mtime, keep the bytes identical.
    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "header.h").write_text(HEADER)

    # Session 2: restart re-enqueues every TU and runs the staleness check.
    # Snapshot the shards BEFORE starting the session — startup indexing is
    # already running when make_client returns, so a later snapshot could
    # race a (buggy) reindex and pass vacuously. Wait until the round
    # completes (save() rewrites the project blob), then re-snapshot: the
    # storm filter must skip the closed TU, leaving its shard untouched.
    before = shard_mtimes(tmp_path)
    project_before = project_mtime(tmp_path)
    c2 = await make_client(executable, tmp_path)
    # save() rewrites the project blob unconditionally each round (see
    # Indexer::save), so its mtime moving proves the round ran.
    assert await poll(lambda: project_mtime(tmp_path) != project_before), (
        "indexing round never ran in session 2"
    )
    after = shard_mtimes(tmp_path)
    await shutdown_client(c2)

    assert after == before, "a same-content touch must not reindex dependents"
