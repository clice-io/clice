"""End-to-end test of the clice extension in a real Zed.

Usage: python editors/zed/tests/e2e.py <zed-executable> <extension.wasm>

Installs the extension into a throwaway Zed data directory and opens a C++
file: Zed must start the clice the extension downloaded from the newest
release, and that clice must resolve its builtin headers from the `lib/clang`
shipped next to it. Then a C file and a CUDA file, each opened alone, must
start the same installation without downloading it again.

`clice` must not be on `PATH`, or the extension uses it instead of
downloading. On Linux, run under `xvfb-run`.
"""

import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

EXTENSION_DIR = pathlib.Path(__file__).resolve().parents[1]
STARTUP_TIMEOUT = 300
# How long a started server has to stay up for the start to count.
SETTLE_SECONDS = 10

STARTED = re.compile(
    r'starting language server process\. binary path: "((?:[^"\\]|\\.)*)"'
)
FAILED = re.compile(r'Failed to start language server "clice".*')


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def step(message):
    print(f"--- {message}", flush=True)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def set_up(root, wasm):
    data = root / "zed"
    installed = data / "extensions" / "installed" / "clice"
    installed.mkdir(parents=True)
    shutil.copy(EXTENSION_DIR / "extension.toml", installed)
    shutil.copy(wasm, installed / "extension.wasm")
    write(
        data / "config" / "settings.json",
        json.dumps(
            {
                "session": {"trust_all_worktrees": True},
                "auto_update": False,
                "telemetry": {"diagnostics": False, "metrics": False},
                "languages": {
                    language: {"language_servers": ["clice", "!clangd", "..."]}
                    for language in ("C++", "C")
                },
            }
        ),
    )
    return data


def make_project(root, file, text):
    """A project holding just `file`, so reopening Zed restores nothing else."""
    project = root / pathlib.Path(file).suffix[1:]
    write(project / file, text)
    command = {
        "directory": str(project),
        "file": str(project / file),
        "arguments": ["clang++", "-std=c++20", "-c", file],
    }
    write(project / "compile_commands.json", json.dumps([command]))
    return project


def stop(zed):
    if sys.platform == "win32":
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(zed.pid)], capture_output=True
        )
    else:
        os.killpg(zed.pid, signal.SIGKILL)
    zed.wait()


def start_server(zed_executable, data, project, file):
    """Opens `file` alone in Zed and returns the clice binary Zed started."""
    log = data / "logs" / "Zed.log"
    log.unlink(missing_ok=True)
    zed = subprocess.Popen(
        [
            zed_executable,
            "--user-data-dir",
            str(data),
            str(project),
            str(project / file),
        ],
        env=os.environ | {"ZED_ALLOW_EMULATED_GPU": "1"},
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=sys.platform != "win32",
    )
    try:
        deadline = time.time() + STARTUP_TIMEOUT
        while time.time() < deadline:
            time.sleep(2)
            text = log.read_text(errors="replace") if log.exists() else ""
            if failure := FAILED.search(text):
                fail(f"{file}: {failure.group(0)}")
            if started := STARTED.search(text):
                binary = pathlib.Path(json.loads(f'"{started.group(1)}"'))
                time.sleep(SETTLE_SECONDS)
                text = log.read_text(errors="replace")
                if failure := FAILED.search(text):
                    fail(f"{file}: {failure.group(0)}")
                return binary
        fail(f"{file}: Zed did not start clice within {STARTUP_TIMEOUT}s")
    finally:
        stop(zed)


def hover(binary, project, file, line, character):
    """Opens `file` in a fresh `clice serve` and returns the hover markdown."""
    server = subprocess.Popen(
        [str(binary), "serve"],
        cwd=project,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )

    def send(message):
        body = json.dumps({"jsonrpc": "2.0"} | message).encode()
        server.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        server.stdin.flush()

    def response(id):
        while True:
            length = 0
            while line := server.stdout.readline().strip():
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            message = json.loads(server.stdout.read(length))
            if message.get("id") == id:
                return message

    try:
        root = project.as_uri()
        send(
            {
                "id": 1,
                "method": "initialize",
                "params": {
                    "processId": None,
                    "rootUri": root,
                    "capabilities": {},
                    "workspaceFolders": [{"uri": root, "name": project.name}],
                },
            }
        )
        response(1)
        send({"method": "initialized", "params": {}})
        uri = (project / file).as_uri()
        send(
            {
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "cpp",
                        "version": 1,
                        "text": (project / file).read_text(),
                    }
                },
            }
        )
        send(
            {
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": line, "character": character},
                },
            }
        )
        result = response(2).get("result")
        return result["contents"]["value"] if result else ""
    finally:
        server.kill()
        server.wait()


def main():
    if len(sys.argv) != 3:
        fail("usage: e2e.py <zed-executable> <extension.wasm>")
    zed_executable, wasm = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
    if shutil.which("clice"):
        fail(
            f"clice is on PATH ({shutil.which('clice')}); the extension would not download"
        )

    root = pathlib.Path(tempfile.mkdtemp(prefix="clice-zed-e2e-"))
    data = set_up(root, wasm)
    work = (data / "extensions" / "work" / "clice").resolve()

    step("open main.cpp: download and start clice")
    project = make_project(
        root, "main.cpp", "#include <stddef.h>\nsize_t size = sizeof(int);\n"
    )
    binary = start_server(zed_executable, data, project, "main.cpp").resolve()
    print(binary)
    if work not in binary.parents or not binary.parent.parent.parent.name.startswith(
        "clice-"
    ):
        fail(f"Zed started {binary}, not a download in {work}")
    if (work / "download").exists():
        fail("the staging directory was left behind")
    installed = binary.stat().st_mtime_ns

    step("hover size_t: builtin headers resolve from the downloaded lib/clang")
    markdown = hover(binary, project, "main.cpp", 1, 2)
    if "size_t" not in markdown:
        fail(f"hover on size_t returned {markdown!r}")

    for file, text in (
        ("util.c", "int twice(int x) { return 2 * x; }\n"),
        ("kernel.cu", "__global__ void kernel() {}\n"),
    ):
        step(f"open {file}: start the installed clice again")
        project = make_project(root, file, text)
        started = start_server(zed_executable, data, project, file).resolve()
        if started != binary or started.stat().st_mtime_ns != installed:
            fail(f"{file}: Zed started {started}, expected the existing {binary}")

    shutil.rmtree(root, ignore_errors=True)
    step("passed")


if __name__ == "__main__":
    main()
