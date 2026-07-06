"""Worker document eviction: opening more files than a stateful worker's
LRU cap must not silently break features on the evicted documents."""

from tests.integration.utils import write_cdb

FILE_COUNT = 18  # one stateful worker holds at most 16 compiled documents


async def test_evicted_document_recovers(client, tmp_path):
    names = []
    for i in range(FILE_COUNT):
        name = f"file_{i:02}.cpp"
        (tmp_path / name).write_text(f"int value_{i} = {i};\n", newline="\n")
        names.append(name)
    write_cdb(tmp_path, names)
    # A single stateful worker so all opens land in one document cache.
    await client.initialize(
        tmp_path,
        initialization_options={"project": {"stateful_worker_count": 1}},
    )

    first_uri, _ = await client.open_and_wait(tmp_path / names[0])
    for name in names[1:]:
        await client.open_and_wait(tmp_path / name)

    # The first file was LRU-evicted from the worker; hover must trigger a
    # recompile instead of silently returning null against the lost AST.
    hover = await client.hover_at(first_uri, 0, 4)
    assert hover is not None, "hover on the evicted document must recover"
