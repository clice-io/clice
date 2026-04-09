import asyncio
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from tests.integration.utils.client import CliceClient


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--executable",
        required=False,
        help="Path to the clice executable.",
    )
    parser.addoption(
        "--mode",
        type=str,
        choices=["pipe", "socket"],
        default="pipe",
        help="The connection mode to use.",
    )
    parser.addoption(
        "--host",
        type=str,
        default="127.0.0.1",
        help="The host to connect to (default: 127.0.0.1)",
    )
    parser.addoption(
        "--port",
        type=int,
        default=50051,
        help="The port to connect to",
    )


@pytest.fixture(scope="session")
def executable(request: pytest.FixtureRequest) -> Path:
    exe = request.config.getoption("--executable")
    if not exe:
        pytest.skip("--executable not provided")

    path = Path(exe)
    if sys.platform.startswith("win") and path.suffix.lower() != ".exe":
        path_exe = path.with_name(path.name + ".exe")
        if path_exe.exists() or not path.exists():
            path = path_exe

    if not path.exists():
        pytest.exit(
            f"Error: clice executable not found at '{exe}'. "
            "Please ensure the path is correct.",
            returncode=64,
        )
    return path.resolve()


@pytest.fixture(scope="session")
def test_data_dir() -> Path:
    path = Path(__file__).parent / "data"
    data_dir = path.resolve()
    _generate_test_data_cdbs(data_dir)
    return data_dir


@pytest.fixture
def workspace(request: pytest.FixtureRequest, test_data_dir: Path) -> Path | None:
    """Resolve workspace path from @pytest.mark.workspace("subdir") marker.

    If the workspace contains a CMakeLists.txt, automatically runs cmake
    to generate compile_commands.json. Returns None if no marker is present.
    """
    marker = request.node.get_closest_marker("workspace")
    if marker is None:
        return None
    if not marker.args or not isinstance(marker.args[0], str):
        raise pytest.UsageError(
            "@pytest.mark.workspace requires a string argument, e.g. "
            '@pytest.mark.workspace("modules/hello_world")'
        )
    path = test_data_dir / marker.args[0]
    if (path / "CMakeLists.txt").exists():
        generate_cdb(path)
    # Clean up persisted index/cache so each test starts fresh.
    clice_dir = path / ".clice"
    if clice_dir.exists():
        shutil.rmtree(clice_dir)
    return path


@pytest.fixture
async def client(
    request: pytest.FixtureRequest, executable: Path, workspace: Path | None
):
    """Spawn clice server, auto-initialize if @pytest.mark.workspace is present."""
    config = request.config
    mode = config.getoption("--mode")

    cmd = [str(executable), "--mode", mode]
    if mode == "socket":
        host = config.getoption("--host")
        port = config.getoption("--port")
        cmd += ["--host", host, "--port", str(port)]

    c = CliceClient()
    await c.start_io(*cmd)

    if workspace is not None:
        await c.initialize(workspace)

    yield c

    await _shutdown_client(c)


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


async def make_client(executable: Path, workspace: Path) -> CliceClient:
    """Spawn a fresh clice server and initialize it. For multi-session tests."""
    c = CliceClient()
    await c.start_io(str(executable), "--mode", "pipe")
    await c.initialize(workspace)
    return c


async def _shutdown_client(c: CliceClient) -> None:
    """Gracefully shut down a client, force-kill if needed."""
    try:
        await asyncio.wait_for(c.shutdown_async(None), timeout=3.0)
    except Exception:
        pass
    try:
        c.exit(None)
    except Exception:
        pass

    await asyncio.sleep(0.3)
    if hasattr(c, "_server") and c._server is not None and c._server.returncode is None:
        c._server.kill()

    try:
        server = getattr(c, "_server", None)
        if server and server.stderr:
            stderr_data = await asyncio.wait_for(server.stderr.read(), timeout=2.0)
            if stderr_data:
                for line in stderr_data.decode("utf-8", errors="replace").splitlines():
                    if "[warn]" in line or "[error]" in line:
                        print(f"[server] {line}", flush=True)
    except Exception:
        pass

    try:
        c._stop_event.set()
        for task in c._async_tasks:
            task.cancel()
        await asyncio.sleep(0.1)
    except Exception:
        pass


shutdown_client = _shutdown_client  # Public alias for multi-session tests


def _generate_test_data_cdbs(data_dir: Path) -> None:
    """Generate compile_commands.json for all static test data directories."""

    def _write(directory: Path, entries: list[dict]) -> None:
        (directory / "compile_commands.json").write_text(json.dumps(entries, indent=2))

    def _entry(directory: Path, source: Path, extra_args: list[str] | None = None):
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
        _write(hw_dir, [_entry(hw_dir, hw_main)])

    # header_context (always regenerate — absolute paths)
    hc_dir = data_dir / "header_context"
    hc_main = hc_dir / "main.cpp"
    if hc_main.exists():
        _write(hc_dir, [_entry(hc_dir, hc_main, [f"-I{hc_dir.as_posix()}"])])

    # multi_context (same file, two configs)
    mc_dir = data_dir / "multi_context"
    mc_main = mc_dir / "main.cpp"
    if mc_main.exists():
        _write(
            mc_dir,
            [
                _entry(mc_dir, mc_main, ["-DCONFIG_A"]),
                _entry(mc_dir, mc_main, ["-DCONFIG_B"]),
            ],
        )

    # include_completion
    ic_dir = data_dir / "include_completion"
    ic_main = ic_dir / "main.cpp"
    if ic_main.exists():
        _write(ic_dir, [_entry(ic_dir, ic_main, ["-I."])])

    # document_links
    dl_dir = data_dir / "document_links"
    dl_main = dl_dir / "main.cpp"
    if dl_main.exists():
        _write(
            dl_dir, [_entry(dl_dir, dl_main, [f"-I{dl_dir.as_posix()}", "-std=c++23"])]
        )

    # pch_test
    pt_dir = data_dir / "pch_test"
    if pt_dir.exists():
        entries = []
        for src_name in ["main.cpp", "no_includes.cpp"]:
            src = pt_dir / src_name
            if src.exists():
                entries.append(_entry(pt_dir, src))
        if entries:
            _write(pt_dir, entries)
