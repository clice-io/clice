"""Integration tests for clice configuration (clice.toml + initializationOptions).

Each workspace's main.cpp references a macro that is only defined when the
rule's `-D<macro>=...` is applied. When rules are applied, compilation is
clean; otherwise an undeclared-identifier diagnostic surfaces.
"""

import pytest

from tests.integration.utils.assertions import (
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
