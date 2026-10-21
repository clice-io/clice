import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "@clice/tools/testing";

const EXCEPT = {
    linux: "a POSIX path is native there",
    darwin: "a POSIX path is native there",
};

// MSYS2, which the Windows runner images ship, spells C:\x as /c/x and keeps
// its runtime in usr/bin.
const MSYS2 = "C:\\msys64\\usr\\bin";

const PATH = Object.keys(process.env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";

/// The PATH of the test runner without the runtime of any Cygwin or MSYS2,
/// such as Git for Windows' usr/bin.
function withoutRuntimes(): string[] {
    return (process.env[PATH] ?? "")
        .split(path.delimiter)
        .filter(
            (dir) =>
                !fs.existsSync(path.join(dir, "msys-2.0.dll")) &&
                !fs.existsSync(path.join(dir, "cygwin1.dll")),
        );
}

/// The database a build run inside MSYS2 writes for the case at `root`.
function database(root: string): string {
    const posix = `/${root.slice(0, 1).toLowerCase()}${root.slice(2).replaceAll("\\", "/")}`;
    const entry = {
        directory: posix,
        file: `${posix}/main.cpp`,
        arguments: ["clang++", `-I${posix}/inc dir`, "-c", `${posix}/main.cpp`],
    };
    return JSON.stringify([entry], null, 2);
}

// The runtime on PATH maps the database's paths: the file has its entry,
// and the entry its include directory.
test("mapped by the runtime", { except: EXCEPT }, async (t) => {
    t.disk.write("compile_commands.json", database(t.root));
    const s = await t.start({
        env: { [PATH]: [MSYS2, ...withoutRuntimes()].join(path.delimiter) },
    });
    await t.show("main.cpp", s.errors("main.cpp"));
});

// Without one, the entries are skipped and the error says why.
test(
    "no runtime on PATH",
    { except: EXCEPT, errors: ["spells its paths POSIX-style"] },
    async (t) => {
        t.disk.write("compile_commands.json", database(t.root));
        const s = await t.start({ env: { [PATH]: withoutRuntimes().join(path.delimiter) } });
        await t.show("main.cpp", s.errors("main.cpp"));
    },
);
