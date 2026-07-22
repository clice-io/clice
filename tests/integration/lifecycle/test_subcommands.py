import subprocess

SUBCOMMANDS = ["serve", "query", "worker", "index", "doc", "lint", "format"]
STUBS = ["index", "doc", "lint", "format"]


def run_clice(executable, *args):
    return subprocess.run(
        [str(executable), *args], capture_output=True, text=True, timeout=30
    )


def test_root_usage_lists_subcommands(executable):
    # Both the bare invocation and --help print the root usage and succeed.
    for args in ([], ["--help"]):
        result = run_clice(executable, *args)
        assert result.returncode == 0
        for name in SUBCOMMANDS:
            assert name in result.stdout


def test_stubs_report_unimplemented(executable):
    # Stubs explain themselves on stderr and exit non-zero: the command is
    # still unavailable and scripts must be able to detect that.
    for name in STUBS:
        result = run_clice(executable, name)
        assert result.returncode == 1
        assert "not implemented" in result.stderr


def test_subcommand_help(executable):
    for name in SUBCOMMANDS:
        result = run_clice(executable, name, "--help")
        assert result.returncode == 0
        assert f"clice {name}" in result.stdout


def test_unknown_subcommand_fails(executable):
    assert run_clice(executable, "bogus").returncode != 0
