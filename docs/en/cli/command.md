# Command

## Overview

`clice command` shows the commands clice compiles a file with, and converts compile commands into those, for other tools to read. It reads the compilation databases and asks the compilers they name; it needs neither an index nor a server.

**Usage**: `clice command <file> [--workspace <dir>] [--configuration <tag>] [--json]`

**Usage**: `clice command convert <compile_commands.json> [--workspace <dir>] [--configuration <tag>]`

**Usage**: `clice command convert [--json] [--workspace <dir>] [--configuration <tag>] -- <compiler> <arguments>...`

## A file's commands

`clice command <file>` lists every entry the compilation databases have for the file, in the order the build ranks them, the first being the one an editor compiles with until the user picks another. Each shows the database holding it, the entry as written, a `command` string split as clice splits it, and the command clice compiles the file with under that entry: the entry edited by the [rules](../guide/configuration.md#rules) of `clice.toml`, then resolved through the compiler it names, which gives the target and the system include directories as the arguments of a `-cc1` command. Where some of those directories hold files clice's compiles do not see — another compiler's builtin headers that clang's own stand in for (GCC's private ones, another clang's), and copies further down the list that compiler never reaches — a `hidden` line counts them for each (see [Toolchain Query](../design/command-resolve.md#toolchain-query)). When the compiler cannot be queried, because it is not installed or rejects the query, the error says why, and the command shown is the driver command clang's own driver reads in the compiler's place, as for a rule's `query_toolchain = false`: a tool built on a compatible Clang reads it as clice does. `--json` prints the same as one JSON object.

The databases are the ones the editor loads for the file: those the rules name, else the `compile_commands.json` at the workspace root, in its direct subdirectories and in the directories between the file and the root. A file without an entry of its own, such as a header, which the editor compiles under the command of a source including it, is reported as having none.

```console
$ clice command src/main.cpp
/home/me/project/build/compile_commands.json:
  directory: /home/me/project/build
  command:   /usr/bin/g++ -std=c++20 -O2 -Werror -o main.o -c ../src/main.cpp
  resolved:  /usr/bin/g++ -cc1 -main-file-name main.cpp -triple x86_64-unknown-linux-gnu -O2 … -internal-isystem /usr/include/c++/13 … -std=c++20 … -x c++ /home/me/project/src/main.cpp
  hidden:    112 of the files in /usr/lib/gcc/x86_64-linux-gnu/13/include
```

The exit code is 0 when the file's entries are listed, 1 when the file does not exist or no database lists it, 2 when the arguments do not parse.

## Converting commands

`clice command convert <compile_commands.json>`, or a directory holding one, prints the database with each entry's command, its `arguments` or `command`, replaced by the `arguments` clice compiles the entry's file with, edited by the rules of the workspace's `clice.toml`. The entry keeps its `file` and `output`; its `directory` is written absolute, a relative one taken from the database's directory; any other field is left out. An entry clice does not compile, such as an assembler source, keeps its command. A written command leaves out the directories of another compiler's builtin headers, whose hidden files it could not carry (see [Known Limitations](../design/command-resolve.md#known-limitations)). `clice command convert -- <compiler> <arguments>...` converts one command, which runs in the current directory and compiles one C-family source file, and prints it as a command line, or with `--json` as a database entry.

The converted commands are `-cc1` commands of the Clang clice is built on: they suit a tool built on a compatible Clang. A command whose compiler cannot be queried stays a driver command instead, the one clang's own driver reads in the compiler's place.

The exit code is 0 when every command is converted; 1 when a compiler cannot be queried, after printing what was converted, the commands it could not query as those driver commands; 1 as well when the database cannot be read or the command compiles no C-family source file or several; 2 when the arguments do not parse.
