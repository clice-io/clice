import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { URI } from "vscode-uri";
import { readTrace } from "./replay.ts";

const RECORD = fileURLToPath(new URL("record.ts", import.meta.url));

function frame(message: object): string {
    const body = JSON.stringify(message);
    return `Content-Length: ${String(Buffer.byteLength(body))}\r\n\r\n${body}`;
}

test("recorded lines replay", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-record-"));
    const trace = path.join(dir, "trace.jsonl");
    const sent = [
        {
            jsonrpc: "2.0",
            id: 0,
            method: "initialize",
            params: {
                processId: 1,
                rootUri: "file:///home/me/C%2B%2B/p",
                rootPath: "/home/me/C++/p",
            },
        },
        {
            jsonrpc: "2.0",
            method: "textDocument/didOpen",
            params: { textDocument: { uri: "file:///home/me/C%2B%2B/p/main.cpp", text: "ü" } },
        },
    ];
    const echo = spawn(process.execPath, [
        RECORD,
        trace,
        process.execPath,
        "-e",
        "process.stdin.pipe(process.stdout)",
    ]);
    let echoed = "";
    echo.stdout.setEncoding("utf8").on("data", (chunk: string) => (echoed += chunk));
    const wire = sent.map(frame).join("");
    echo.stdin.write(wire.slice(0, 10));
    echo.stdin.end(wire.slice(10));
    const status = await new Promise((resolve) => echo.on("close", resolve));
    expect(status).toBe(0);
    expect(echoed).toBe(wire);
    const root = path.join(dir, "copy");
    const played = readTrace(trace, root).map(({ message }) => message);
    expect(played).toHaveLength(2);
    expect(played[0]?.params).toMatchObject({ processId: process.pid, rootPath: root });
    expect(played[1]?.params).toMatchObject({
        textDocument: { uri: `${URI.file(root).toString()}/main.cpp`, text: "ü" },
    });
    fs.rmSync(dir, { recursive: true });
});
