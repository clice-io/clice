import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "@clice/tools/testing";

const EXCEPT = {
    linux: "a POSIX path is native there",
    darwin: "a POSIX path is native there",
};

// Git for Windows, which every Windows runner image ships, carries an MSYS2
// runtime: it spells C:\x as /c/x.
const GIT_RUNTIME = path.join(
    process.env["ProgramFiles"] ?? "C:\\Program Files",
    "Git",
    "usr",
    "bin",
);

const PATH = Object.keys(process.env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";

/// The runner's PATH without the runtime of any Cygwin or MSYS2.
function withoutRuntimes(): string[] {
    return (process.env[PATH] ?? "")
        .split(path.delimiter)
        .filter(
            (dir) =>
                !fs.existsSync(path.join(dir, "msys-2.0.dll")) &&
                !fs.existsSync(path.join(dir, "cygwin1.dll")),
        );
}

function posix(file: string): string {
    return `/${file.slice(0, 1).toLowerCase()}${file.slice(2).replaceAll("\\", "/")}`;
}

/// The clang++ on PATH, spelled as a POSIX path without `.exe`.
function compiler(): string {
    const clang = withoutRuntimes().find((dir) => fs.existsSync(path.join(dir, "clang++.exe")));
    if (clang === undefined) {
        throw new Error("no clang++.exe on PATH");
    }
    return posix(path.join(clang, "clang++"));
}

/// The database a build run inside MSYS2 writes for the case at `root`.
function database(root: string, cc = compiler()): string {
    const entry = {
        directory: posix(root),
        file: posix(path.join(root, "main.cpp")),
        arguments: [
            cc,
            `-I${posix(path.join(root, "inc dir"))}`,
            "-include",
            posix(path.join(root, "inc dir", "forced.h")),
            "-c",
            posix(path.join(root, "main.cpp")),
        ],
    };
    return JSON.stringify([entry], null, 2);
}

/// The same entry as a `command` string, quoted as sh quotes it.
function commandDatabase(root: string): string {
    const quoted = (word: string) => `'${word}'`;
    const entry = {
        directory: posix(root),
        file: posix(path.join(root, "main.cpp")),
        command: [
            quoted(compiler()),
            quoted(`-I${posix(path.join(root, "inc dir"))}`),
            "-include",
            quoted(posix(path.join(root, "inc dir", "forced.h"))),
            "-c",
            quoted(posix(path.join(root, "main.cpp"))),
        ].join(" "),
    };
    return JSON.stringify([entry], null, 2);
}

// The runtime on PATH maps the database's paths: the file has its entry,
// the entry its compiler, its include directory and its forced include.
test("mapped by the runtime", { except: EXCEPT }, async (t) => {
    t.disk.write("compile_commands.json", database(t.root));
    const s = await t.start({
        env: { [PATH]: [GIT_RUNTIME, ...withoutRuntimes()].join(path.delimiter) },
    });
    await t.show("main.cpp", s.errors("main.cpp"));
});

// The command splits as sh splits it, at the quotes around a path with a
// space, which Windows' rules keep in the word.
test("a command quoted for sh", { except: EXCEPT }, async (t) => {
    t.disk.write("compile_commands.json", commandDatabase(t.root));
    const s = await t.start({
        env: { [PATH]: [GIT_RUNTIME, ...withoutRuntimes()].join(path.delimiter) },
    });
    await t.show("main.cpp", s.errors("main.cpp"));
});

// A runtime ahead on PATH that cannot list its mounts is passed over for the
// next one.
test("a broken runtime ahead", { except: EXCEPT }, async (t) => {
    t.disk.write("compile_commands.json", database(t.root));
    t.disk.write("broken/msys-2.0.dll", "");
    const broken = path.join(t.root, "broken");
    const s = await t.start({
        env: { [PATH]: [broken, GIT_RUNTIME, ...withoutRuntimes()].join(path.delimiter) },
    });
    await t.show("main.cpp", s.errors("main.cpp"));
});

// `clice command convert` writes the Windows paths clice reads. The compiler
// does not exist, so the run fails after printing the database.
test(
    "converted",
    { except: EXCEPT, errors: ["Driver .*missing-cc not found", "could not be resolved"] },
    async (t) => {
        t.disk.write("compile_commands.json", database(t.root, path.join(t.root, "missing-cc")));
        await t.cli("command convert compile_commands.json", {
            env: { [PATH]: [GIT_RUNTIME, ...withoutRuntimes()].join(path.delimiter) },
        });
    },
);

// Without one, the database cannot be read and the user is told why. The
// copy's POSIX spelling shows as ${WS} too.
test("no runtime on PATH", { except: EXCEPT }, async (t) => {
    t.disk.write("compile_commands.json", database(t.root));
    const s = await t.start({ env: { [PATH]: withoutRuntimes().join(path.delimiter) } });
    await t.show("main.cpp", s.errors("main.cpp"));
    const told = await s.guidance();
    await t.show(
        "guidance",
        told.map((message) => message.replaceAll(posix(t.root), "${WS}")),
    );
});
