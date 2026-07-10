"""Integration tests for clice configuration (clice.toml + initializationOptions).

Each workspace's main.cpp references a macro that is only defined when the
rule's `-D<macro>=...` is applied. When rules are applied, compilation is
clean; otherwise an undeclared-identifier diagnostic surfaces.
"""

import pytest
from lsprotocol.types import DiagnosticSeverity

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.checks import assert_no_anomaly
from tests.tools.checks import (
    assert_clean_compile,
    assert_has_errors,
    get_errors,
)


@pytest.mark.workspace("config_rules_no_config")
async def test_baseline_without_rules(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert_has_errors(client, uri, "Expected diagnostics without any rules applied")
    errors = get_errors(client.diagnostics[uri])
    assert any("FROM_INIT" in (d.message or "") for d in errors), (
        f"Expected a diagnostic referencing FROM_INIT, got: {errors}"
    )


@pytest.mark.workspace("config_rules_toml")
async def test_rules_from_toml(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert_clean_compile(client, uri)

    symbols = await client.document_symbols(uri)
    assert symbols, "Expected document symbols for value()/main()"
    hover = await client.hover_at(uri, line=4, character=4)  # on 'main'
    assert hover is not None


@pytest.mark.workspace("config_rules_no_config")
@pytest.mark.init_options(
    {"rules": [{"patterns": ["**/*.cpp"], "append": ["-DFROM_INIT=1"]}]}
)
async def test_rules_from_init_options(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert_clean_compile(client, uri)


@pytest.mark.workspace("config_rules_toml")
@pytest.mark.init_options(
    {"rules": [{"patterns": ["**/*.cpp"], "append": ["-DUNRELATED"]}]}
)
async def test_init_options_replaces_toml_rules(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert_has_errors(
        client, uri, "initializationOptions should have overridden clice.toml rules"
    )
    errors = get_errors(client.diagnostics[uri])
    assert any("FROM_TOML" in (d.message or "") for d in errors), (
        f"Expected FROM_TOML diagnostic after override, got: {errors}"
    )


@pytest.mark.workspace("config_rules_no_config")
@pytest.mark.init_options(
    {"rules": [{"patterns": ["**/does_not_match.cpp"], "append": ["-DFROM_INIT=1"]}]}
)
async def test_rules_pattern_mismatch(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert_has_errors(client, uri, "Rule pattern should not have matched main.cpp")


async def test_config_type_error_diagnostic(executable, tmp_path):
    # Wrong value type → Error diagnostic on the clice.toml URI; the config
    # falls back to defaults. (Line/column pinpointing awaits the kotatsu
    # TOML error-location feature — see config_tests.cpp.)
    (tmp_path / "clice.toml").write_text('[project]\nclang_tidy = "yes"\n')
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    client = await make_client(executable, tmp_path)
    try:
        toml_uri = client.path_to_uri(tmp_path / "clice.toml")
        await client.wait_diagnostics(toml_uri, timeout=10)
        diags = client.diagnostics[toml_uri]
        assert len(diags) == 1, f"expected one config diagnostic: {diags}"
        assert diags[0].severity == DiagnosticSeverity.Error
        assert "clang_tidy" in diags[0].message
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)


async def test_config_unknown_key_diagnostic(executable, tmp_path):
    # Typo'd key → Warning diagnostic; the rest of the config still applies.
    (tmp_path / "clice.toml").write_text("[project]\nclang_tdy = true\n")
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    client = await make_client(executable, tmp_path)
    try:
        toml_uri = client.path_to_uri(tmp_path / "clice.toml")
        await client.wait_diagnostics(toml_uri, timeout=10)
        diags = client.diagnostics[toml_uri]
        assert len(diags) == 1, f"expected one config diagnostic: {diags}"
        assert diags[0].severity == DiagnosticSeverity.Warning
        assert "clang_tdy" in diags[0].message
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)


async def test_config_diagnostic_clears_after_fix(executable, tmp_path):
    (tmp_path / "clice.toml").write_text('[project]\nclang_tidy = "yes"\n')
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    client = await make_client(executable, tmp_path)
    try:
        toml_uri = client.path_to_uri(tmp_path / "clice.toml")
        await client.wait_diagnostics(toml_uri, timeout=10)
        assert client.diagnostics[toml_uri], "broken config should be diagnosed"
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)

    # Fix the config and restart — the new session publishes an empty list
    # for the config URI so stale markers clear.
    (tmp_path / "clice.toml").write_text("[project]\nclang_tidy = true\n")
    client = await make_client(executable, tmp_path)
    try:
        toml_uri = client.path_to_uri(tmp_path / "clice.toml")
        await client.wait_diagnostics(toml_uri, timeout=10)
        assert client.diagnostics[toml_uri] == [], "fixed config must clear diagnostics"
        assert_no_anomaly(client, tmp_path)
    finally:
        await shutdown_client(client)
