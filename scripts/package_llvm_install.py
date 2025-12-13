import argparse
import tarfile
from pathlib import Path


def create_archive(install_dir: Path, archive_path: Path, compresslevel: int) -> None:
    if not install_dir.is_dir():
        raise FileNotFoundError(f"Install directory not found: {install_dir}")
    archive_path.parent.mkdir(parents=True, exist_ok=True)

    # compresslevel 6 is a good speed/ratio trade-off for CI.
    with tarfile.open(archive_path, mode="w:gz", compresslevel=compresslevel) as tf:
        # Keep top-level directory name stable in the archive.
        tf.add(install_dir, arcname="build-install")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Pack LLVM install dir to .tar.gz")
    parser.add_argument(
        "--install-dir",
        default=".llvm/build-install",
        type=Path,
        help="Path to LLVM install dir (default: .llvm/build-install)",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="Output archive path (e.g., llvm-windows.tar.gz)",
    )
    parser.add_argument(
        "--compresslevel",
        type=int,
        default=6,
        help="gzip compress level (0-9, default: 6)",
    )
    args = parser.parse_args()
    level = max(0, min(9, args.compresslevel))
    create_archive(args.install_dir, args.output, level)
    print(f"Created archive: {args.output}")


if __name__ == "__main__":
    main()
