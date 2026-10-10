/// `clice query --fresh` and `clice index` with a server running on the
/// workspace: the server holds the writer lock, so they ask it to index —
/// or give up when it cannot be asked.

import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

const PIN_CACHE = '[project]\ncache_dir = "${workspace}/.clice"\n';

const project = serve("tiny", { files: { "clice.toml": PIN_CACHE } });

interface Symbols {
    result?: { symbols: { name: string }[] };
    error?: string;
}

async function symbolSearch(s: Serve, name: string, ...args: string[]) {
    const run = await s.cli("query", "--method", "symbolSearch", "--query", name, ...args);
    expect(run.stdout, `stderr: ${run.stderr}`).not.toBe("");
    const answer = run.json as Symbols;
    return {
        ...answer,
        status: run.status,
        names: answer.result?.symbols.map((symbol) => symbol.name),
    };
}

async function serverIndexed(s: Serve, name: string): Promise<void> {
    await s.indexed();
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    expect(
        symbols.some((symbol) => symbol.name === name),
        "server never indexed",
    ).toBe(true);
}

project("asks the running server to index", async ({ s }) => {
    await serverIndexed(s, "add");
    expect(s.workspace.exists(".clice/server.json")).toBe(true);

    // The server holds the writer lock, so the batch command delegates.
    const delegated = await s.cli("index", "--workers", "2");
    expect(delegated.status, `stderr: ${delegated.stderr}`).toBe(0);
    expect(delegated.stdout).toContain("through the running clice server");

    s.disk.edit("main.cpp", { before: "int main()", insert: "int extra() { return 7; }\n" });
    const fresh = await symbolSearch(s, "extra", "--fresh");
    expect(fresh.status).toBe(0);
    expect(fresh.names).toEqual(["extra"]);

    const persisted = await symbolSearch(s, "extra");
    expect(persisted.names).toEqual(["extra"]);

    await s.stop();
    expect(s.workspace.exists(".clice/server.json")).toBe(false);
});

serve("tiny", { files: { "clice.toml": PIN_CACHE }, databases: false })(
    "asked index finds later database",
    async ({ s }) => {
        // The server records its control endpoint as it starts: any reply
        // comes after.
        await s.sync();
        expect(s.workspace.exists(".clice/server.json")).toBe(true);

        const empty = await s.cli("index", "--workers", "2");
        expect(empty.stderr).toContain("has no translation units");

        s.disk.database(s.manifest);
        const delegated = await s.cli("index", "--workers", "2");
        expect(delegated.status, `stderr: ${delegated.stderr}`).toBe(0);
        await serverIndexed(s, "add");
    },
);

project("refuses a writer it cannot ask", async ({ s }) => {
    await serverIndexed(s, "add");

    // A lock holder without a record (a batch run, a server of another
    // build) cannot be asked: the commands that need the writer give up.
    s.disk.rm(".clice/server.json");
    const refused = await s.cli("index", "--workers", "2");
    expect(refused.status).toBe(1);
    expect(refused.stderr).toContain("holds the index writer lock");

    const fresh = await symbolSearch(s, "add", "--fresh");
    expect(fresh.status).toBe(1);
    expect(fresh.error).toContain("holds the index writer lock");

    // Reads never wait for the writer; they see the disk, which the
    // settled server's indexing round has reached.
    expect((await symbolSearch(s, "add")).names).toContain("add");
});

serve("tiny", {
    files: {
        "clice.toml": [
            "[project]",
            'cache_dir = "${workspace}/.clice"',
            "",
            "[[rules]]",
            'configuration = "debug"',
            'patterns = ["**/*.cpp"]',
            'append = ["-DDEBUG"]',
            "",
            "[[rules]]",
            'configuration = "release"',
            'patterns = ["**/*.cpp"]',
            'append = ["-DRELEASE"]',
            "",
        ].join("\n"),
    },
    launch: { args: ["serve", "--configuration", "debug"] },
})("delegation keeps the configuration", async ({ s }) => {
    await serverIndexed(s, "add");

    // The server indexes one configuration; asking it for another is
    // refused rather than answered with the wrong build.
    const other = await s.cli("index", "--configuration", "release");
    expect(other.status).toBe(1);
    expect(other.stderr).toContain("configuration 'debug'");

    const same = await s.cli("index", "--configuration", "debug");
    expect(same.status, `stderr: ${same.stderr}`).toBe(0);
    expect(same.stdout).toContain("through the running clice server");
});
