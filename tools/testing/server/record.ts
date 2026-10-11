/// Record what an editor sends a server, for a recorded-session case:
///
///     node tools/testing/server/record.ts <trace.jsonl> <clice> serve
///
/// stands in for the server the editor launches (behind a wrapper script
/// where the editor takes only an executable), passes every byte through
/// both ways, and writes each message the editor sent as one trace line.

import { spawn } from "node:child_process";
import * as fs from "node:fs";

const [trace, executable, ...args] = process.argv.slice(2);
if (trace === undefined || executable === undefined) {
    console.error("usage: record.ts <trace.jsonl> <server> [args...]");
    process.exit(2);
}

const server = spawn(executable, args, { stdio: ["pipe", "inherit", "inherit"] });
const out = fs.openSync(trace, "w");
const start = performance.now();
let pending = Buffer.alloc(0);

process.stdin.on("data", (chunk: Buffer) => {
    server.stdin.write(chunk);
    pending = Buffer.concat([pending, chunk]);
    for (;;) {
        const end = pending.indexOf("\r\n\r\n");
        const length = /Content-Length: *(\d+)/i.exec(pending.subarray(0, end).toString("ascii"));
        if (end === -1 || length?.[1] === undefined) {
            return;
        }
        const body = end + 4 + Number(length[1]);
        if (pending.length < body) {
            return;
        }
        const msg = pending.subarray(end + 4, body).toString("utf8");
        fs.writeSync(
            out,
            `${JSON.stringify({ ts: Math.round(performance.now() - start), msg })}\n`,
        );
        pending = pending.subarray(body);
    }
});
process.stdin.on("end", () => server.stdin.end());
server.on("exit", (code, signal) => {
    fs.closeSync(out);
    process.exit(code ?? (signal === null ? 1 : 128));
});
