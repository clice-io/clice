import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { Workspace } from "../../client/workspace.ts";
import { gateLogs } from "./serve.ts";

function logged(lines: readonly string[]): Workspace {
    const workspace = new Workspace(fs.mkdtempSync(path.join(os.tmpdir(), "clice-gate-")));
    workspace.write(".clice/logs/session/master.log", `${lines.join("\n")}\n`);
    return workspace;
}

function crash(workspace: Workspace, file: string): string[] {
    return [
        `[t] [warning] [thread 1] [pool.cpp:39] [SF-0] clice worker crashed in: clice/worker/query:Hover ${workspace.displayPath(file)}`,
        "[t] [error] [thread 1] [pool.cpp:578] [anomaly:WorkerCrash] Worker SF-0 killed by signal 4 (SIGILL)",
    ];
}

describe("gateLogs", () => {
    const hover = { crash: { request: "query:Hover", file: "a.cpp" } };

    test("the declared crash passes", () => {
        const workspace = logged([]);
        workspace.write(
            ".clice/logs/session/master.log",
            `${crash(workspace, "a.cpp").join("\n")}\n`,
        );
        expect(gateLogs(workspace, { servers: [hover] }, false)).toEqual([]);
        workspace.remove();
    });

    test("another crash fails", () => {
        const workspace = logged([]);
        workspace.write(
            ".clice/logs/session/master.log",
            `${[...crash(workspace, "a.cpp"), ...crash(workspace, "b.cpp")].join("\n")}\n`,
        );
        const [error] = gateLogs(workspace, { servers: [hover] }, false) as Error[];
        expect(error?.message).toMatch(/nothing declared/);
        workspace.remove();
    });

    test("a declared crash that never happened fails", () => {
        const workspace = logged(["[t] [info] [thread 1] [x.cpp:1] nothing"]);
        const [error] = gateLogs(
            workspace,
            { servers: [hover, { crash: { request: "query:Hover", file: "b.cpp" } }] },
            false,
        ) as Error[];
        expect(error?.message).toMatch(/never happened:\n.*a.cpp\n.*b.cpp/);
        workspace.remove();
    });

    test("a command's declared crash passes", () => {
        const workspace = logged([]);
        workspace.write(
            ".clice/logs/session/lint.log",
            `${[
                `[t] [warning] [thread 1] [pool.cpp:39] [SL-0] clice worker crashed in: clice/worker/tuRun ${workspace.displayPath("a.cpp")}`,
                "[t] [error] [thread 1] [pool.cpp:575] [anomaly:WorkerCrash] Worker SL-0 killed by signal 4 (crash streak: 0)",
            ].join("\n")}\n`,
        );
        expect(gateLogs(workspace, {}, false)).not.toEqual([]);
        const tuRun = { crash: { request: "tuRun", file: "a.cpp" } };
        expect(gateLogs(workspace, { commands: [tuRun] }, false)).toEqual([]);
        workspace.remove();
    });

    test("undeclared error lines fail", () => {
        const workspace = logged(["[t] [error] [thread 1] [x.cpp:1] something broke"]);
        expect(gateLogs(workspace, {}, false)).toHaveLength(1);
        expect(gateLogs(workspace, { errors: [/something broke/] }, false)).toEqual([]);
        workspace.remove();
    });
});

test("an anomaly nothing declared fails", () => {
    const workspace = logged([
        "[t] [error] [thread 1] [pool.cpp:578] [anomaly:WorkerCrash] Worker SF-0 killed by signal 9 (SIGKILL)",
    ]);
    const errors = gateLogs(workspace, { errors: [/anomaly/] }, false) as Error[];
    expect(errors.map((error) => error.message)).toEqual([
        expect.stringMatching(/explain 0 worker crashes/),
    ]);
    workspace.remove();
});
