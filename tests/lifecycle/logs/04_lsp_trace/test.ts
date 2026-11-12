import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// With CLICE_LSP_TRACE set, the server writes every message it receives
// (`<-`) and sends (`->`), each on a line of its own after the time, to
// trace.log in its session directory: the handshake first, the exit last.
test(async (t) => {
    const s = await t.start({ env: { CLICE_LSP_TRACE: "1" } });
    await s.diagnostics("main.cpp");
    await s.stop();
    const traces = t.disk.list(".clice/logs/*/trace.log");
    expect(traces).toHaveLength(1);
    const messages = t.disk
        .read(traces[0] ?? "")
        .split("\n")
        .filter((line) => line !== "")
        .map((line) => {
            const [, direction, json] = /^\[[^\]]+\] (<-|->) (.*)$/.exec(line) ?? [];
            const message = JSON.parse(json ?? "null") as { id?: number; method?: string };
            return `${direction ?? "?"} ${message.method ?? `reply ${String(message.id)}`}`;
        });
    expect(messages.slice(0, 2)).toEqual(["<- initialize", "-> reply 0"]);
    expect(messages.filter((message) => message.startsWith("<- textDocument/didOpen"))).toEqual([
        "<- textDocument/didOpen",
    ]);
    expect(messages.at(-1)).toBe("<- exit");
});
