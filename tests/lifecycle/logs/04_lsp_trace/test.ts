import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// With CLICE_LSP_TRACE set, the server writes every message it receives
// (`<-`) and sends (`->`), each on a line of its own after the time, to
// trace.log in its session directory: what the client sent in its order,
// and a reply to each of its requests among what the server sent.

interface Traced {
    direction: string;
    id?: number;
    method?: string;
}

/// The messages of the session's one trace, after its server stopped.
function traced(t: TestContext): Traced[] {
    const traces = t.disk.list(".clice/logs/*/trace.log");
    expect(traces).toHaveLength(1);
    const messages = t.disk
        .read(traces[0] ?? "")
        .split("\n")
        .filter((line) => line !== "")
        .map((line): Traced => {
            const [, direction = line, json = "{}"] = /^\[[^\]]+\] (<-|->) (.*)$/.exec(line) ?? [];
            return { direction, ...(JSON.parse(json) as Omit<Traced, "direction">) };
        });
    expect(messages.filter(({ direction }) => direction !== "<-" && direction !== "->")).toEqual(
        [],
    );
    const ids = (list: Traced[]) =>
        list.flatMap(({ id }) => (id === undefined ? [] : [id])).sort((a, b) => a - b);
    const sent = messages.filter(
        ({ direction, method }) => direction === "<-" && method !== undefined,
    );
    const replies = messages.filter(
        ({ direction, method }) => direction === "->" && method === undefined,
    );
    expect(ids(replies)).toEqual(ids(sent));
    return sent;
}

test(async (t) => {
    const s = await t.start({ env: { CLICE_LSP_TRACE: "1" } });
    await s.diagnostics("main.cpp");
    await s.stop();
    await t.show(
        "the client's messages",
        traced(t).map(({ method }) => method ?? ""),
    );
});

// `serve --workspace` opens its project, and its log, before any client
// speaks: the trace opens with the log all the same.
test("a server ready before its client", async (t) => {
    const s = await t.spawn({
        args: ["serve", `--workspace=${t.root}`],
        env: { CLICE_LSP_TRACE: "1" },
    });
    s.open("main.cpp", { pull: false });
    await t.show("hover", s.hover(t.at("main.cpp", "int |add")));
    await s.stop();
    await t.show(
        "the client's messages",
        traced(t).map(({ method }) => method ?? ""),
    );
});
