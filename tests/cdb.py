"""Compilation database generation for test fixtures.

Stdlib-only so it can be used both by pytest (conftest.py) and by
standalone scripts (tests/editors/prepare.py) without pulling in
pytest/pygls dependencies.
"""

import json
import shutil
import subprocess
from pathlib import Path


def generate_cdb(workspace: Path) -> None:
    """Generate compile_commands.json using CMake with Ninja backend."""
    cmake = shutil.which("cmake")
    if cmake is None:
        raise RuntimeError("cmake executable not found in PATH")
    toolchain = Path(__file__).resolve().parent.parent / "cmake" / "toolchain.cmake"
    cmd = [
        cmake,
        "-G",
        "Ninja",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-S",
        str(workspace),
        "-B",
        str(workspace / "build"),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(f"cmake failed:\n{result.stderr}")


def generate_test_data_cdbs(data_dir: Path) -> None:
    """Generate compile_commands.json for all static test data directories."""

    def write(directory: Path, entries: list[dict]) -> None:
        (directory / "compile_commands.json").write_text(json.dumps(entries, indent=2))

    def entry(directory: Path, source: Path, extra_args: list[str] | None = None):
        args = ["clang++", "-std=c++17", "-fsyntax-only"]
        if extra_args:
            args.extend(extra_args)
        args.append(source.as_posix())
        return {
            "directory": directory.as_posix(),
            "file": source.as_posix(),
            "arguments": args,
        }

    # hello_world
    hw_dir = data_dir / "hello_world"
    hw_main = hw_dir / "main.cpp"
    if hw_main.exists():
        write(hw_dir, [entry(hw_dir, hw_main)])

    # header_context (always regenerate — absolute paths)
    hc_dir = data_dir / "header_context"
    hc_main = hc_dir / "main.cpp"
    if hc_main.exists():
        write(hc_dir, [entry(hc_dir, hc_main, [f"-I{hc_dir.as_posix()}"])])

    # multi_context (same file, two configs)
    mc_dir = data_dir / "multi_context"
    mc_main = mc_dir / "main.cpp"
    if mc_main.exists():
        write(
            mc_dir,
            [
                entry(mc_dir, mc_main, ["-DCONFIG_A"]),
                entry(mc_dir, mc_main, ["-DCONFIG_B"]),
            ],
        )

    # include_completion
    ic_dir = data_dir / "include_completion"
    ic_main = ic_dir / "main.cpp"
    if ic_main.exists():
        write(ic_dir, [entry(ic_dir, ic_main, ["-I."])])

    # document_links
    dl_dir = data_dir / "document_links"
    dl_main = dl_dir / "main.cpp"
    if dl_main.exists():
        write(
            dl_dir, [entry(dl_dir, dl_main, [f"-I{dl_dir.as_posix()}", "-std=c++23"])]
        )

    # config_rules_toml / config_rules_no_config — rules tests must start
    # from a CDB that does NOT include the flag the rule will append, so the
    # rule's effect is observable through diagnostics.
    for name in ("config_rules_toml", "config_rules_no_config"):
        cr_dir = data_dir / name
        cr_main = cr_dir / "main.cpp"
        if cr_main.exists():
            write(cr_dir, [entry(cr_dir, cr_main)])

    # formatting
    fmt_dir = data_dir / "formatting"
    fmt_main = fmt_dir / "main.cpp"
    if fmt_main.exists():
        write(fmt_dir, [entry(fmt_dir, fmt_main)])

    # pch_test
    pt_dir = data_dir / "pch_test"
    if pt_dir.exists():
        entries = []
        for src_name in ["main.cpp", "no_includes.cpp"]:
            src = pt_dir / src_name
            if src.exists():
                entries.append(entry(pt_dir, src))
        if entries:
            write(pt_dir, entries)
