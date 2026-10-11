/// Rename from the persisted index: `clice refactor rename` plans the edits
/// and writes them unless a conflict or a stale index stands in the way,
/// and the editor's prepareRename/rename answer from the same engine.

import { basename } from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { runProcess } from "@clice/tools/client";
import { applyTextEdits } from "@clice/tools/client/edits";
import { canonicalUri, Workspace } from "@clice/tools/workspace";
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
    name: string;
    newName: string;
    heuristic: boolean;
}

interface Note {
    file: string;
    line: number;
    column: number;
    name: string;
    reason: string;
    text: string;
}

interface Entry {
    line: number | null;
    locator: string;
    name: string;
    kind: string;
    symbol: string;
    newName: string;
    edits: number;
    conflicts: string[];
    warnings: string[];
    error: string | null;
    candidates: { name: string; line: number; symbol: string }[] | null;
}

interface Renamed {
    applied: boolean;
    renames: Entry[];
    files: { file: string; edits: Edit[] }[];
    unconfirmed: Note[];
    conflicts: string[];
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

/// The answer, its first rename as `entry`, whose error is the answer's
/// when the command itself has none.
async function rename(
    ws: Workspace,
    ...args: string[]
): Promise<Answer & { status: number | null; entry?: Entry }> {
    const run = await runClice("refactor", "rename", "--workspace", ws.root, ...args);
    expect(run.stdout, `stderr: ${run.stderr}`).not.toBe("");
    const answer = JSON.parse(run.stdout) as Answer;
    const entry = answer.result?.renames[0];
    const error = answer.error ?? entry?.error ?? undefined;
    return { ...answer, status: run.status, ...(entry && { entry }), ...(error && { error }) };
}

/// Rename by a list of renames written to a file in the workspace.
async function renameList(ws: Workspace, list: string, ...args: string[]) {
    ws.write("renames.txt", list);
    return rename(ws, "--spec", ws.path("renames.txt"), ...args);
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

    const answer = await rename(ws, "--symbol", "compute", "--to", "evaluate", "--dry-run");
    expect(answer.status).toBe(0);
    const renamed = answer.result!;
    expect(answer.entry!.kind).toBe("Function");
    expect(renamed.applied).toBe(false);
    expect(places(renamed)).toEqual(["a.h:2:5", "main.cpp:2:5", "main.cpp:4:20", "other.cpp:2:22"]);
    expect(renamed.unconfirmed.map((note) => `${basename(note.file)}:${note.line}`)).toEqual([
        "a.h:4",
        "main.cpp:4",
    ]);
    expect(renamed.unconfirmed[0]!.text).toBe("#define CALL compute(1)");
    expect(answer.entry!.conflicts).toEqual([]);
    expect(ws.read("main.cpp")).toBe(MAIN);
});

test("writes the edits", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const answer = await rename(ws, "--symbol", "Widget", "--to", "Gadget");
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

    const answer = await rename(ws, "--symbol", "compute", "--to", "Widget");
    expect(answer.status).toBe(1);
    expect(answer.result!.applied).toBe(false);
    expect(answer.entry!.conflicts.join("\n")).toContain("already declared in the same scope");
    expect(ws.read("main.cpp")).toBe(MAIN);
});

test("a stale index blocks the write", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);
    ws.write("other.cpp", OTHER + "int more() { return compute(4); }\n");

    const stale = await rename(ws, "--symbol", "compute", "--to", "evaluate");
    expect(stale.status).toBe(1);
    expect(stale.result!.applied).toBe(false);
    expect(stale.result!.stale.map((file) => basename(file))).toContain("other.cpp");
    expect(ws.read("main.cpp")).toBe(MAIN);

    const fresh = await rename(
        ws,
        "--symbol",
        "compute",
        "--to",
        "evaluate",
        "--fresh",
        "--dry-run",
    );
    expect(fresh.status, fresh.error).toBe(0);
    expect(places(fresh.result!)).toContain("other.cpp:3:21");
});

test("refuses what it cannot rename", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const keyword = await rename(ws, "--symbol", "compute", "--to", "int");
    expect(keyword.status).toBe(1);
    expect(keyword.entry!.conflicts).toEqual(["`int` is a keyword"]);

    const macro = await rename(ws, "--symbol", "CALL", "--to", "INVOKE");
    expect(macro.status).toBe(1);
    expect(macro.error).toContain("macro");

    expect((await rename(ws, "--symbol", "compute")).error).toContain("--to");
    const unknown = await runClice("refactor", "extract", "--workspace", ws.root);
    expect(unknown.status).toBe(1);
    expect(unknown.stdout).toContain("unknown refactoring");
});

test("names its symbol exactly", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    for (const near of ["comput", "Compute"]) {
        const answer = await rename(ws, "--symbol", near, "--to", "evaluate", "--dry-run");
        expect(answer.status).toBe(1);
        expect(answer.error).toBe("symbol not found");
    }
    const pattern = await rename(ws, "--symbol", "comp*", "--to", "evaluate", "--dry-run");
    expect(pattern.status).toBe(1);
    expect(pattern.error).toContain("whole name");

    // The place a name is written names the symbol too; a file the index
    // never held is named with the answer.
    const byPlace = await rename(ws, "--symbol", "other.cpp:2:22", "--to", "evaluate", "--dry-run");
    expect(byPlace.status, byPlace.error).toBe(0);
    expect(byPlace.entry!.name).toBe("compute");
    ws.write("new.cpp", "int fresh() { return 1; }\n");
    const unindexed = await rename(ws, "--symbol", "new.cpp:1:5", "--to", "evaluate");
    expect(unindexed.status).toBe(1);
    expect(unindexed.stale.map((file) => basename(file))).toContain("new.cpp");
});

test("lists the symbols a name spells", async ({ session }) => {
    const ws = session.tmpdir();
    const text =
        "namespace a { int value; }\nnamespace b { int value; }\nint use() { return a::value + b::value; }\n";
    ws.write("main.cpp", text);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    await index(ws);

    const ambiguous = await rename(ws, "--symbol", "value", "--to", "amount", "--dry-run");
    expect(ambiguous.status).toBe(1);
    expect(ambiguous.error).toContain("2 symbols match");
    expect(ambiguous.entry!.candidates!.map((c) => c.name)).toEqual(["value", "value"]);

    const scoped = await rename(ws, "--symbol", "a::value", "--to", "amount", "--dry-run");
    expect(scoped.status, scoped.error).toBe(0);
    expect(places(scoped.result!)).toEqual(["main.cpp:1:19", "main.cpp:3:23"]);
    const [, candidate] = ambiguous.entry!.candidates!;
    const picked = await rename(ws, "--symbol", candidate!.symbol, "--to", "amount");
    expect(picked.status, picked.error).toBe(0);
    expect(places(picked.result!)[0]).toBe(`main.cpp:${candidate!.line}:19`);
    expect(picked.result!.files.flatMap((file) => file.edits)).toHaveLength(2);
});

test("keeps other scopes' functions apart", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "src/main.cpp",
        [
            "namespace a { int f(int x) { return x; } }",
            "namespace b { int f(double x) { return 1; } }",
            "struct S { int g(int x); int g(double x); };",
            "int S::g(int x) { return x; }",
            "int S::g(double x) { return 1; }",
            "int use(S s) { return a::f(1) + b::f(2.0) + s.g(1) + s.g(2.0); }",
            "",
        ].join("\n"),
    );
    ws.write(
        "src/one.cpp",
        "static int helper(int x) { return x; }\nint one() { return helper(1); }\n",
    );
    ws.write(
        "src/two.cpp",
        "static int helper(double x) { return 2; }\nint two() { return helper(2); }\n",
    );
    ws.write(
        "src/mixed.cpp",
        [
            "int mix(int x) { return x; }",
            "static int mix(double x) { return 2; }",
            "int use_mix() { return mix(1) + mix(2.0); }",
            "",
        ].join("\n"),
    );
    ws.writeCDB(["src/main.cpp", "src/one.cpp", "src/two.cpp", "src/mixed.cpp"]);
    ws.pinCacheDir();
    await index(ws);

    const scopes = await rename(ws, "--symbol", "f", "--to", "h", "--dry-run");
    expect(scopes.error).toContain("2 symbols match");
    const units = await rename(ws, "--symbol", "helper path:src/", "--to", "h", "--dry-run");
    expect(units.error, "two units' static functions are no overload set").toContain(
        "2 symbols match",
    );

    // One file's overloads stay a set whatever their linkage.
    const mixed = await rename(ws, "--symbol", "mix path:src/mixed.cpp", "--to", "h", "--dry-run");
    expect(mixed.status, mixed.error).toBe(0);
    expect(places(mixed.result!)).toEqual([
        "mixed.cpp:1:5",
        "mixed.cpp:2:12",
        "mixed.cpp:3:24",
        "mixed.cpp:3:33",
    ]);

    const methods = await rename(ws, "--symbol", "S::g", "--to", "h", "--dry-run");
    expect(methods.status, methods.error).toBe(0);
    expect(places(methods.result!)).toEqual([
        "main.cpp:3:16",
        "main.cpp:3:30",
        "main.cpp:4:8",
        "main.cpp:5:8",
        "main.cpp:6:47",
        "main.cpp:6:56",
    ]);
});

test("renames the overloads a name spells", async ({ session }) => {
    const ws = session.tmpdir();
    const text = [
        "int scale(int x) { return x; }",
        "double scale(double x) { return x; }",
        "template <typename T> T twice(T v) { return scale(v) * 2; }",
        "int use() { return scale(1) + twice(2); }",
        "",
    ].join("\n");
    ws.write("main.cpp", text);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    await index(ws);

    // Renamed together, the template's open call keeps naming them.
    const all = await rename(ws, "--symbol", "scale", "--to", "resize", "--dry-run");
    expect(all.status, all.error).toBe(0);
    expect(places(all.result!)).toEqual([
        "main.cpp:1:5",
        "main.cpp:2:8",
        "main.cpp:3:45",
        "main.cpp:4:20",
    ]);
    expect(all.result!.unconfirmed).toEqual([]);

    const definition = await runClice(
        "query",
        "--workspace",
        ws.root,
        "--method",
        "definition",
        "--symbol",
        "main.cpp:2:8",
    );
    const { symbol } = (JSON.parse(definition.stdout) as { result: { symbol: string } }).result;
    const one = await rename(ws, "--symbol", symbol, "--to", "resize", "--dry-run");
    expect(one.status, one.error).toBe(0);
    expect(places(one.result!)).toEqual(["main.cpp:2:8"]);
    expect(one.result!.unconfirmed.map((note) => `${note.line}:${note.column}`)).toEqual(["3:45"]);
});

test("renames a list at once", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const list = [
        "// one run, one index",
        "compute => evaluate",
        "",
        "Widget => Gadget",
        "  use   =>   apply  ",
        "",
    ].join("\n");
    const answer = await renameList(ws, list);
    expect(answer.status, answer.error).toBe(0);
    const renamed = answer.result!;
    expect(renamed.applied).toBe(true);
    expect(renamed.renames.map((entry) => [entry.line, entry.locator, entry.newName])).toEqual([
        [2, "compute", "evaluate"],
        [4, "Widget", "Gadget"],
        [5, "use", "apply"],
    ]);
    expect(ws.read("a.h")).toBe(
        HEADER.replace("int compute", "int evaluate").replace(
            "Widget { Widget()",
            "Gadget { Gadget()",
        ),
    );
    expect(ws.read("main.cpp")).toBe(
        MAIN.replace("int compute", "int evaluate")
            .replace("Widget::Widget()", "Gadget::Gadget()")
            .replace("int use() { return compute(2)", "int apply() { return evaluate(2)")
            .replace("Widget w; return use()", "Gadget w; return apply()"),
    );
    expect(ws.read("other.cpp")).toBe(OTHER.replace("compute", "evaluate"));

    // The list is also read from stdin.
    await index(ws);
    const piped = await runProcess(
        cliceExecutable(),
        ["refactor", "rename", "--workspace", ws.root, "--spec", "-", "--dry-run"],
        { timeout: 120_000, input: "evaluate => compute\n" },
    );
    expect(piped.status, piped.stdout).toBe(0);
    const edits = (JSON.parse(piped.stdout) as Answer).result!.files.flatMap((file) => file.edits);
    expect(edits.every((edit) => edit.name === "evaluate" && edit.newName === "compute")).toBe(
        true,
    );
});

test("swaps and chains names", async ({ session }) => {
    const ws = session.tmpdir();
    const text = "int alpha;\nint beta;\nint gamma() { return alpha + beta; }\n";
    ws.write("main.cpp", text);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    await index(ws);

    const answer = await renameList(ws, "alpha => beta\nbeta => gamma\ngamma => alpha\n");
    expect(answer.status, answer.error).toBe(0);
    expect(ws.read("main.cpp")).toBe(
        "int beta;\nint gamma;\nint alpha() { return beta + gamma; }\n",
    );
});

test("one stopped rename stops the list", async ({ session }) => {
    const ws = writeProject(session);
    await index(ws);

    const answer = await renameList(ws, "compute => evaluate\nWidget => evaluate\nnope => none\n");
    expect(answer.status).toBe(1);
    const [compute, widget, nope] = answer.result!.renames;
    expect(compute!.conflicts.join("\n")).toContain("already declared in the same scope");
    expect(widget!.conflicts.join("\n")).toContain("already declared in the same scope");
    expect(nope!.error).toBe("symbol not found");
    expect(answer.result!.applied).toBe(false);
    expect(ws.read("main.cpp")).toBe(MAIN);

    const malformed = await renameList(ws, "compute evaluate\n");
    expect(malformed.status).toBe(1);
    expect(malformed.error).toBe("line 1: expected `<locator> => <new name>`");
    const both = await rename(ws, "--spec", "-", "--symbol", "compute", "--to", "evaluate");
    expect(both.error).toContain("--spec");
});

/// The edits a rename reply makes to `file`, and the buffer version they
/// were computed for.
function changeOf(edit: proto.WorkspaceEdit | null, ws: Workspace, file: string) {
    for (const change of edit?.documentChanges ?? []) {
        if ("textDocument" in change && canonicalUri(change.textDocument.uri) === ws.uri(file)) {
            const edits = change.edits.filter((item): item is proto.TextEdit => "newText" in item);
            return { version: change.textDocument.version, edits };
        }
    }
    return null;
}

test("renames from the editor", async ({ session }) => {
    const ws = writeProject(session);
    const client = await session.spawn(ws).initialize(ws);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "again"), "other.cpp not indexed").toBe(true);

    const prepared = await client.prepareRenameAt(uri, 1, 6);
    expect(prepared).toEqual({
        range: { start: { line: 1, character: 4 }, end: { line: 1, character: 11 } },
        placeholder: "compute",
    });

    const notices: string[] = [];
    client.onNotification(proto.ShowMessageNotification.type, (params) => {
        notices.push(params.message);
    });
    const edit = await client.renameAt(uri, 1, 6, "evaluate");
    const main = changeOf(edit, ws, "main.cpp");
    expect(main?.version).toBe(0);
    expect(applyTextEdits(MAIN, main!.edits)).toBe(
        MAIN.replace("int compute", "int evaluate").replace("compute(2)", "evaluate(2)"),
    );
    const header = changeOf(edit, ws, "a.h");
    expect(header?.version).toBeNull();
    expect(applyTextEdits(HEADER, header!.edits)).toBe(
        HEADER.replace("int compute", "int evaluate"),
    );
    const other = changeOf(edit, ws, "other.cpp");
    expect(applyTextEdits(OTHER, other!.edits)).toBe(OTHER.replace("compute", "evaluate"));
    await expect.poll(() => notices.join("\n")).toContain("#define CALL compute(1)");
});

test("the editor hears why not", async ({ session }) => {
    const ws = writeProject(session);
    const client = await session.spawn(ws).initialize(ws);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "again"), "other.cpp not indexed").toBe(true);

    await expect(client.prepareRenameAt(uri, 3, 33)).rejects.toThrow("macro");
    await expect(client.renameAt(uri, 1, 6, "use")).resolves.not.toBeNull();
    await expect(client.renameAt(uri, 1, 6, "Widget")).rejects.toThrow(
        "already declared in the same scope",
    );
    expect(await client.prepareRenameAt(uri, 0, 0)).toBeNull();
});

test("a rootless server refuses up front", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int compute();\nint use() { return compute(); }\n");
    await client.initialize(workspace, { folders: [] });
    const [uri] = await client.openAndWait("main.cpp");

    await expect(client.prepareRenameAt(uri, 0, 5)).rejects.toThrow("workspace folder");
    await expect(client.renameAt(uri, 0, 5, "evaluate")).rejects.toThrow("workspace folder");
});
