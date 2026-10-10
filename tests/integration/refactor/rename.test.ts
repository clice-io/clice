/// Rename from the persisted index: `clice refactor rename` plans the edits
/// and writes them unless a conflict or a stale index stands in the way,
/// and the editor's prepareRename/rename answer from the same engine.

import { basename } from "node:path";
import { runProcess } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const HEADER = [
    "#pragma once",
    "int compute(int x);",
    "struct Widget { Widget(); int value; };",
    "#define CALL compute(1)",
    "",
].join("\n");
const MAIN = [
    '#include "a.h"',
    "int compute(int x) { return x; }",
    "Widget::Widget() : value(0) {}",
    "int use() { return compute(2) + CALL; }",
    "int main() { Widget w; return use() + w.value; }",
    "",
].join("\n");
const OTHER = ['#include "a.h"', "int again() { return compute(3); }", ""].join("\n");

interface Edit {
    line: number;
    column: number;
    heuristic: boolean;
}

interface Note {
    file: string;
    line: number;
    column: number;
    reason: string;
    text: string;
}

interface Renamed {
    name: string;
    kind: string;
    newName: string;
    applied: boolean;
    files: { file: string; edits: Edit[] }[];
    unconfirmed: Note[];
    conflicts: string[];
    warnings: string[];
    stale: string[];
}

interface Answer {
    result?: Renamed;
    error?: string;
    stale: string[];
}

function writeProject(session: { tmpdir(): Workspace }): Workspace {
    const ws = session.tmpdir();
    ws.write("a.h", HEADER);
    ws.write("main.cpp", MAIN);
    ws.write("other.cpp", OTHER);
    ws.writeCDB(["main.cpp", "other.cpp"]);
    ws.pinCacheDir();
    return ws;
}

function runClice(...args: string[]) {
    return runProcess(cliceExecutable(), args, { timeout: 120_000 });
}

async function index(ws: Workspace) {
    const run = await runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(run.status, run.stderr).toBe(0);
}

async function rename(
    ws: Workspace,
    ...args: string[]
): Promise<Answer & { status: number | null }> {
    const run = await runClice("refactor", "rename", "--workspace", ws.root, ...args);
    expect(run.stdout, `stderr: ${run.stderr}`).not.toBe("");
    return { ...(JSON.parse(run.stdout) as Answer), status: run.status };
}

/// `file:line:column` of every planned edit, files in name order.
function places(renamed: Renamed): string[] {
    return renamed.files.flatMap(({ file, edits }) =>
        edits.map((edit) => `${basename(file)}:${edit.line}:${edit.column}`),
    );
}

test("plans without writing", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const answer = await rename(ws, "--name", "compute", "--to", "evaluate", "--dry-run");
    expect(answer.status).toBe(0);
    const renamed = answer.result!;
    expect(renamed.kind).toBe("Function");
    expect(renamed.applied).toBe(false);
    expect(places(renamed)).toEqual(["a.h:2:5", "main.cpp:2:5", "main.cpp:4:20", "other.cpp:2:22"]);
    expect(renamed.unconfirmed.map((note) => `${basename(note.file)}:${note.line}`)).toEqual([
        "a.h:4",
        "main.cpp:4",
    ]);
    expect(renamed.unconfirmed[0]!.text).toBe("#define CALL compute(1)");
    expect(renamed.conflicts).toEqual([]);
    expect(ws.read("main.cpp")).toBe(MAIN);
});

test("writes the edits", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const answer = await rename(ws, "--name", "Widget", "--to", "Gadget");
    expect(answer.status, answer.error).toBe(0);
    expect(answer.result!.applied).toBe(true);
    expect(ws.read("a.h")).toBe(HEADER.replace("Widget { Widget()", "Gadget { Gadget()"));
    expect(ws.read("main.cpp")).toBe(
        MAIN.replace("Widget::Widget()", "Gadget::Gadget()").replace("Widget w;", "Gadget w;"),
    );
    expect(ws.read("other.cpp")).toBe(OTHER);
});

test("conflicts block the write", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const answer = await rename(ws, "--name", "compute", "--to", "Widget");
    expect(answer.status).toBe(1);
    expect(answer.result!.applied).toBe(false);
    expect(answer.result!.conflicts.join("\n")).toContain("already declared in the same scope");
    expect(ws.read("main.cpp")).toBe(MAIN);
});

test("a stale index blocks the write", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);
    ws.write("other.cpp", OTHER + "int more() { return compute(4); }\n");

    const stale = await rename(ws, "--name", "compute", "--to", "evaluate");
    expect(stale.status).toBe(1);
    expect(stale.result!.applied).toBe(false);
    expect(stale.result!.stale.map((file) => basename(file))).toContain("other.cpp");
    expect(ws.read("main.cpp")).toBe(MAIN);

    const fresh = await rename(ws, "--name", "compute", "--to", "evaluate", "--fresh", "--dry-run");
    expect(fresh.status, fresh.error).toBe(0);
    expect(places(fresh.result!)).toContain("other.cpp:3:21");
});

test("refuses what it cannot rename", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const keyword = await rename(ws, "--name", "compute", "--to", "int");
    expect(keyword.status).toBe(1);
    expect(keyword.result!.conflicts).toEqual(["`int` is a keyword"]);

    const macro = await rename(ws, "--name", "CALL", "--to", "INVOKE");
    expect(macro.status).toBe(1);
    expect(macro.error).toContain("macro");

    expect((await rename(ws, "--name", "compute")).error).toContain("--to");
    const unknown = await runClice("refactor", "extract", "--workspace", ws.root);
    expect(unknown.status).toBe(1);
    expect(unknown.stdout).toContain("unknown refactoring");
});
