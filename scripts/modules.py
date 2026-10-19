"""Regenerate modules/ from modules/demand.cpp and the libraries' headers.

Each configuration CI builds is generated on a machine that builds it (Linux
makes its own, arm64 Linux and both Windows; macOS arm64 its own and x64),
then all of them are merged into modules/:

  python scripts/modules.py [--clice PATH] generate <configuration>...
  python scripts/modules.py [--clice PATH] merge

A configuration's wrapping lands in build/modules/<configuration>; merge
reads every one of them there. Any clice runs it, by default the one the
tree builds; after a library's upgrade, one built before it, as the new
headers need not build the old modules/.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "modules")

# name: (build type, target triple, condition)
CONFIGURATIONS = {
    "linux-x64": (
        "RelWithDebInfo",
        "x86_64-unknown-linux-gnu",
        "defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)",
    ),
    "linux-x64-debug": (
        "Debug",
        "x86_64-unknown-linux-gnu",
        "defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)",
    ),
    "linux-arm64": (
        "RelWithDebInfo",
        "aarch64-unknown-linux-gnu",
        "defined(__linux__) && defined(__aarch64__) && defined(NDEBUG)",
    ),
    "macos-arm64": (
        "RelWithDebInfo",
        "aarch64-apple-darwin",
        "defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)",
    ),
    "macos-arm64-debug": (
        "Debug",
        "aarch64-apple-darwin",
        "defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)",
    ),
    "macos-x64": (
        "RelWithDebInfo",
        "x86_64-apple-darwin",
        "defined(__APPLE__) && defined(__x86_64__) && defined(NDEBUG)",
    ),
    "windows-x64": (
        "RelWithDebInfo",
        "x86_64-w64-mingw32",
        "defined(_WIN32) && defined(__x86_64__) && defined(NDEBUG)",
    ),
    "windows-arm64": (
        "RelWithDebInfo",
        "aarch64-w64-mingw32",
        "defined(_WIN32) && defined(__aarch64__) && defined(NDEBUG)",
    ),
}

HOSTS = {
    ("linux", "x86_64"): "x86_64-unknown-linux-gnu",
    ("linux", "aarch64"): "aarch64-unknown-linux-gnu",
    ("darwin", "arm64"): "aarch64-apple-darwin",
    ("darwin", "x86_64"): "x86_64-apple-darwin",
}


def bazel(*args, capture=False):
    command = ["npx", "bazel", *args]
    if capture:
        return subprocess.run(
            command, cwd=ROOT, check=True, capture_output=True, text=True
        ).stdout.strip()
    subprocess.run(command, cwd=ROOT, check=True)


def repo(external, name):
    link = f"{external}/{name}"
    return sorted({f"{link}/**", f"{os.path.realpath(link)}/**"})


def partition(output_base, clice):
    """The libraries' partition on this machine: Bazel's output base and the
    repository contents cache differ by platform, so the globs come from
    where the build put each library. The index reads the compiler's own
    headers from clice's resource directory."""
    external = f"{output_base}/external"
    xclang = f"{external}/xclang++xclang+xclang_*"
    libclang = f"{external}/xclang++xclang+libclang_*"
    deps = [
        f"{libclang}/include/{tree}/**"
        for tree in ("llvm", "llvm-c", "clang", "clang-c", "clang-tidy")
    ]
    for name in (
        "simdjson+",
        "kotatsu+",
        "croaring+",
        "tomlplusplus+",
        "spdlog+",
        "lmdb+",
    ):
        deps += repo(external, name)
    return {
        "modules": [
            {"name": "libc", "files": [f"{xclang}/**/include/c++/v1/*.h"]},
            {
                "name": "std",
                # <triple>/c++/v1 holds the target's __config_site.
                "files": [
                    f"{xclang}/**/include/c++/v1/**",
                    f"{xclang}/**/include/*/c++/v1/**",
                ],
                "external": True,
            },
            {
                "name": "libc",
                "files": [
                    f"{xclang}/**",
                    f"{libclang}/lib/clang/**",
                    f"{os.path.dirname(os.path.dirname(clice))}/lib/clang/**",
                    "/Applications/**",
                    "/Library/Developer/**",
                ],
                "textual": True,
                "provides": "std.compat",
            },
            {"name": "deps", "files": deps},
        ]
    }


def generate(name, clice):
    build_type, triple, _ = CONFIGURATIONS[name]
    host = HOSTS[(sys.platform, os.uname().machine)]
    flags = [f"--config={build_type}"]
    if triple != host:
        flags.append(f"--platforms=@xclang//platforms:{triple}")

    # A configuration's directory exists only after its last generation
    # went through: merge takes whatever is there.
    out = os.path.join(OUT, name)
    work = os.path.join(OUT, f"{name}.index")
    shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    events = f"{work}/events.json"
    bazel(
        "build",
        *flags,
        "--aspects=@compdb//:compdb.bzl%compdb",
        # Compiled for real: clice indexes a unit that does not compile.
        "--output_groups=+compdb",
        f"--build_event_json_file={events}",
        "//modules:demand",
    )
    execroot = bazel("info", "execution_root", capture=True)
    output_base = bazel("info", "output_base", capture=True)
    with open(events) as stream:
        fragments = set(
            re.findall(
                r'"uri":"file://([^"]*/modules/demand\.compdb\.json)"', stream.read()
            )
        )
    if len(fragments) != 1:
        raise SystemExit(
            f"{name}: the compile command of //modules:demand: {fragments}"
        )
    with open(urllib.parse.unquote(fragments.pop())) as fragment:
        commands = fragment.read().replace(
            "__COMPDB_EXECROOT__", execroot.replace("\\", "\\\\")
        )
    with open(f"{work}/compile_commands.json", "w") as database:
        database.write(commands)
    with open(f"{work}/clice.toml", "w") as config:
        config.write('[[rules]]\ncompile_commands = ["compile_commands.json"]\n')
    with open(f"{work}/partition.json", "w") as spec:
        json.dump(partition(output_base, clice), spec, indent=2)
    std = [
        os.path.dirname(path)
        for path in glob.glob(
            f"{output_base}/external/xclang++xclang+xclang_*/**/share/libc++/v1/std.cppm",
            recursive=True,
        )
        if f"/{triple}/" in path
    ]
    if len(std) != 1:
        raise SystemExit(f"{name}: libc++'s module sources for {triple}: {std}")

    subprocess.run([clice, "index", "--workspace", work], check=True)
    plan = subprocess.run(
        [
            clice,
            "modularize",
            "--workspace",
            work,
            "--scope",
            "/**",
            "--partition",
            f"{work}/partition.json",
            "--std",
            std[0],
            "--out",
            f"{work}/wrapping",
            "--no-mirrors",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    # A header the partition leaves to the program, or modules sharing a
    # TU-local entity: the partition needs fixing.
    if warnings := json.loads(plan)["wrapping"]["warnings"]:
        raise SystemExit("\n".join(f"{name}: {warning}" for warning in warnings))
    os.rename(f"{work}/wrapping", out)


def merge(clice):
    configurations = [
        {"name": name, "condition": condition, "out": name}
        for name, (_, _, condition) in CONFIGURATIONS.items()
    ]
    missing = [
        entry["name"]
        for entry in configurations
        if not os.path.isdir(os.path.join(OUT, entry["name"]))
    ]
    if missing:
        raise SystemExit(f"not generated: {', '.join(missing)}")
    with open(os.path.join(OUT, "merge.json"), "w") as spec:
        json.dump({"configurations": configurations}, spec, indent=2)
    subprocess.run(
        [
            clice,
            "modularize",
            "merge",
            os.path.join(OUT, "merge.json"),
            "--out",
            os.path.join(ROOT, "modules"),
            "--no-mirrors",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--clice", default=os.path.join(ROOT, "build/RelWithDebInfo/bin/bin/clice")
    )
    commands = parser.add_subparsers(dest="command", required=True)
    generating = commands.add_parser("generate")
    generating.add_argument("configurations", nargs="+", choices=CONFIGURATIONS)
    commands.add_parser("merge")
    args = parser.parse_args()
    clice = os.path.abspath(args.clice)
    if args.command == "generate":
        for name in args.configurations:
            generate(name, clice)
    else:
        merge(clice)


if __name__ == "__main__":
    main()
