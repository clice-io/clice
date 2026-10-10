/// Build configurations from the command line: batch indexing writes one
/// library per configuration, and scripted commands refuse a name no rule
/// declares (the server's side is in serve/compilation/configuration).

import * as fs from "node:fs";
import * as path from "node:path";
import { runProcess } from "@clice/tools/client";
import { materialize } from "@clice/tools/project";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function runClice(...args: string[]) {
    return runProcess(cliceExecutable(), args, { timeout: 120_000 });
}

/// Whether the configuration's index library holds a database.
function hasLibrary(workspace: Workspace, configuration: string): boolean {
    const library = workspace.indexLibrary(configuration);
    return library !== undefined && fs.existsSync(path.join(library, "index.mdb"));
}

test("batch index per configuration", async ({ session }) => {
    const workspace = session.tmpdir();
    materialize("layouts/two_configurations", workspace);
    const release = ["--workspace", workspace.root, "--configuration", "release"];

    const indexed = await runClice("index", ...release, "--workers", "2");
    expect(indexed.status, `stderr: ${indexed.stderr}`).toBe(0);
    expect(indexed.stdout).toContain("Indexed 4 translation units in");
    expect(hasLibrary(workspace, "release")).toBe(true);

    const stats = await runClice("index", "--stats", ...release);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Configuration: release");
    expect(stats.stdout).toContain("Translation units: 4");

    // The default configuration's library was never written.
    const missing = await runClice("index", "--stats", "--workspace", workspace.root);
    expect(missing.status).toBe(1);
    expect(missing.stderr).toContain("No index cache");

    const debug = await runClice("index", "--workspace", workspace.root, "--workers", "2");
    expect(debug.status, `stderr: ${debug.stderr}`).toBe(0);
    expect(debug.stdout).toContain("Indexed 4 translation units in");
    expect(hasLibrary(workspace, "debug")).toBe(true);
    const both = await runClice("index", "--stats", "--workspace", workspace.root);
    expect(both.stdout).toContain("Configuration: debug");
    expect(both.stdout).toContain("Translation units: 4");
});

test("scripted commands reject unknown names", async ({ session }) => {
    // The server falls back so an editor always starts; a batch or
    // inspection run with a misspelt name must fail, not report success
    // for another configuration.
    const workspace = session.tmpdir();
    materialize("layouts/two_configurations", workspace);
    const unknown = ["--workspace", workspace.root, "--configuration", "nope"];
    for (const [command, status] of [
        [["index", "--workers", "2"], 1],
        [["index", "--stats"], 1],
        [["lint", "--workers", "2"], 2],
    ] as const) {
        const run = await runClice(...command, ...unknown);
        expect(run.status, command.join(" ")).toBe(status);
        expect(run.stderr, command.join(" ")).toContain("names no rule's configuration");
    }
    const gated = path.join(workspace.root, "gated.cpp");
    expect((await runClice("inspect", "--configuration", "nope", "hover", gated)).status).toBe(1);
    expect(
        (
            await runClice(
                "inspect",
                "--configuration",
                "release",
                "--flags",
                '["clang++"]',
                "hover",
                gated,
            )
        ).status,
        "--flags replaces the rules the name would select among",
    ).toBe(1);
    const inspect = async (...args: string[]) => {
        const run = await runClice("inspect", ...args, "hover", gated);
        expect(run.status, `stderr: ${run.stderr}`).toBe(0);
        const output = JSON.parse(run.stdout) as {
            files: Record<string, { diagnostics?: string[] | null }>;
        };
        return Object.values(output.files).flatMap((file) => file.diagnostics ?? []);
    };
    expect(await inspect(), "the default configuration lacks RELEASE").toEqual(
        expect.arrayContaining([expect.stringContaining("missing RELEASE")]),
    );
    expect(await inspect("--configuration", "release")).toEqual([]);
});
