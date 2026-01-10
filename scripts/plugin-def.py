# read include/Server/PluginDef.h and substitute CLICE_PLUGIN_DEF_HASH in include/Server/Plugin.h
# hash is in `sha256:<hash>` format.

import hashlib
import re
from pathlib import Path
import sys

#

clice_apis = []

# all of the files in `include/`, other than `include/Server/Plugin.h`
for path in Path("include/").glob("**/*.h"):
    if path.name == "Plugin.h":
        continue
    clice_apis.append(path)

clice_apis = [
    # the dependencies of the clice.
    Path("config/llvm-manifest.json"),
    # the clice C/C++ sources.
    *sorted(clice_apis),
]


def fatal(message: str):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def sha256sum(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


def clice_api_content():
    content = []
    content.append("#if 0")
    for path in clice_apis:
        with path.open("r") as file:
            content.append(f"// begin of {path}")
            content.append(file.read())
            content.append(f"// end of {path}")
    content.append("#endif")
    return "\n".join(content)


def plugin_def_hash():
    hash_val = sha256sum(clice_apis)
    return f"sha256:{hash_val}"


def update():
    hash_val = plugin_def_hash()
    with open("include/Server/Plugin.h", "r") as file:
        content = file.read()
    content = re.sub(
        r"#define CLICE_PLUGIN_DEF_HASH .*",
        f'#define CLICE_PLUGIN_DEF_HASH "{hash_val}"',
        content,
    )
    with open("include/Server/Plugin.h", "w") as file:
        file.write(content)


def check():
    hash_val = plugin_def_hash()
    with open("include/Server/Plugin.h", "r") as file:
        content = file.read()
    match = re.search(r'#define CLICE_PLUGIN_DEF_HASH "(.*)"', content)
    if match is None:
        fatal("plugin def hash not found in include/Server/Plugin.h")
    if match.group(1) != hash_val:
        fatal(
            f"plugin def hash mismatch in include/Server/Plugin.h, expected: {hash_val}, actual: {match.group(1)}"
        )
    print(f"plugin def hash is up to date: {hash_val}")


def main():
    if len(sys.argv) > 1:
        if sys.argv[1] == "update":
            update()
            check()
        elif sys.argv[1] == "check":
            check()
        elif sys.argv[1] == "content":
            print(clice_api_content())
        else:
            fatal(
                f"invalid command: {sys.argv[1]}, expected: update, check",
            )
    else:
        fatal("no command provided, expected: update, check")


if __name__ == "__main__":
    main()
