import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

const PATH = Object.keys(process.env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";
const PTXAS = process.platform === "win32" ? "ptxas.exe" : "ptxas";

// A CUDA header is C-family code though clang's table of suffixes does
// not list .cuh, and so is a file of any suffix the command's -x names a
// C-family language for; the entries of other languages a build lists
// (a resource script, an .ixx without -x) stay as written. clang's own
// driver, reading the missing nvcc's entry, finds a CUDA installation by
// a ptxas on PATH, which only some machines have.
test({ errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    const cudaless = (process.env[PATH] ?? "")
        .split(path.delimiter)
        .filter((dir) => !fs.existsSync(path.join(dir, PTXAS)));
    await t.cli("command convert compile_commands.json", {
        env: { [PATH]: cudaless.join(path.delimiter) },
    });
});
