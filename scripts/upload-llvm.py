import os
import platform
import subprocess
import sys
from pathlib import Path


def detect_pattern() -> str:
    sys_name = platform.system().lower()
    if "windows" in sys_name:
        return "*win*"
    if "linux" in sys_name:
        return "*linux*"
    if "darwin" in sys_name:
        return "*mac*"
    raise RuntimeError(f"Unsupported platform for artifact pattern: {sys_name}")


def require_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def download_artifacts(run_id: str, pattern: str, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "gh",
        "run",
        "download",
        run_id,
        "--pattern",
        pattern,
        "--dir",
        str(out_dir),
    ]
    subprocess.run(cmd, check=True)


def list_contents(root: Path) -> None:
    if not root.exists():
        print(f"No artifacts directory found at {root}")
        return
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root.parent)
        suffix = "/" if path.is_dir() else ""
        print(rel.as_posix() + suffix)


def main() -> None:
    run_id = require_env("RUN_ID")
    _ = require_env("GH_TOKEN")  # gh picks this up implicitly

    pattern = detect_pattern()
    artifacts_dir = Path("artifacts")

    print(f"Downloading artifacts for run {run_id} with pattern {pattern}...")
    download_artifacts(run_id, pattern, artifacts_dir)

    print("Downloaded contents:")
    list_contents(artifacts_dir)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
