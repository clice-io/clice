import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: .*a DLL it loads is missing";

const PATH = Object.keys(process.env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";

// A program of Git for Windows' MSYS2, which every Windows runner image
// ships, stands in for a Cygwin or MSYS2 compiler: it cannot start without
// msys-2.0.dll.
const PROGRAM = path.join(
    process.env["ProgramFiles"] ?? "C:\\Program Files",
    "Git",
    "usr",
    "bin",
    "env.exe",
);

// The compiler cannot start, its runtime missing from PATH: the error says
// to put it there.
test(
    {
        errors: [MISSING],
        except: { linux: "only Windows loads a DLL from PATH", darwin: "no DLLs there" },
    },
    async (t) => {
        const gcc = path.join(t.root, "gcc.exe");
        fs.copyFileSync(PROGRAM, gcc);
        const entry = { directory: t.root, file: "main.cpp", arguments: [gcc, "-c", "main.cpp"] };
        t.disk.write("compile_commands.json", JSON.stringify([entry], null, 2));
        const runtimeless = (process.env[PATH] ?? "")
            .split(path.delimiter)
            .filter((dir) => !fs.existsSync(path.join(dir, "msys-2.0.dll")));
        await t.cli("index", { env: { [PATH]: runtimeless.join(path.delimiter) } });
    },
);
