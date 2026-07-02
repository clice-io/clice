"""Guidance diagnostics for files compiled with guessed compile commands.

When a file has no compilation database entry, clice compiles it with a
synthesized fallback command. If that produces file-not-found errors, a
file-top warning explains the situation; an exact CDB match never gets it.
"""

from lsprotocol.types import DiagnosticSeverity

from tests.conftest import make_client, shutdown_client
from tests.integration.utils import write_cdb
from tests.integration.utils.assertions import assert_no_anomaly, guidance_messages

GUIDANCE_CODE = "inferred-compile-command"

BROKEN_INCLUDE = '#include "no_such_header.h"\nint main() { return 0; }\n'


def guidance_diags(client, uri):
    return [d for d in client.diagnostics.get(uri, []) if d.code == GUIDANCE_CODE]


def file_not_found_diags(client, uri):
    return [
        d for d in client.diagnostics.get(uri, []) if d.code == "err_pp_file_not_found"
    ]


async def test_fallback_guidance_lifecycle(executable, tmp_path):
    (tmp_path / "main.cpp").write_text(BROKEN_INCLUDE)

    # Phase 1: no CDB — fallback command, broken include → guidance at the top.
    client = await make_client(executable, tmp_path)
    try:
        uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
        assert file_not_found_diags(client, uri), "broken include should surface"
        guidance = guidance_diags(client, uri)
        assert len(guidance) == 1, f"expected one guidance diagnostic: {guidance}"
        assert guidance[0].severity == DiagnosticSeverity.Warning
        assert guidance[0].range.start.line == 0
        assert guidance[0].source == "clice"
        # The missing CDB is also announced via window/logMessage guidance.
        assert any("compile_commands.json" in m for m in guidance_messages(client))
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)

    # Phase 2: provide a CDB and restart — the include error remains, the
    # guidance diagnostic must disappear (exact CDB match never gets it).
    write_cdb(tmp_path, ["main.cpp"])
    client = await make_client(executable, tmp_path)
    try:
        uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
        assert file_not_found_diags(client, uri), "include is still broken"
        assert not guidance_diags(client, uri), (
            "CDB-matched files must not get the inferred-command guidance"
        )
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)


async def test_fallback_applies_rule_appends(executable, tmp_path):
    # Without a CDB, include paths supplied via clice.toml rules must reach
    # the synthesized fallback command.
    (tmp_path / "inc").mkdir()
    (tmp_path / "inc" / "dep.h").write_text("#pragma once\nconstexpr int dep = 1;\n")
    (tmp_path / "main.cpp").write_text('#include "dep.h"\nint main() { return dep; }\n')
    include_dir = (tmp_path / "inc").as_posix()
    (tmp_path / "clice.toml").write_text(
        f'[[rules]]\npatterns = ["**/*.cpp"]\nappend = ["-I{include_dir}"]\n'
    )

    client = await make_client(executable, tmp_path)
    try:
        uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
        assert not file_not_found_diags(client, uri), (
            "rule -I must reach the fallback command"
        )
        assert not guidance_diags(client, uri)
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)


async def test_fallback_clean_no_guidance(executable, tmp_path):
    # A guessed command that works produces no guidance noise.
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    client = await make_client(executable, tmp_path)
    try:
        uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
        assert not guidance_diags(client, uri)
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)
