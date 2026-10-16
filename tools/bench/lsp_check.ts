/// What the checks of one LSP feature over real files share: the lines of a
/// document, findings grouped by kind, and the servers they ask, clice and
/// clangd started the same way.

import * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { withTimeout } from "../promise.ts";

/// Line starts of `text`, a line ending at `\n` (its `\r` before it
/// belongs to the break, not the line).
export class Lines {
    readonly starts: number[] = [0];
    readonly text: string;

    constructor(text: string) {
        this.text = text;
        for (let i = 0; i < text.length; i += 1) {
            if (text[i] === "\n") {
                this.starts.push(i + 1);
            }
        }
    }

    get count(): number {
        return this.starts.length;
    }

    /// The length of line `line`, its break excluded.
    length(line: number): number {
        const begin = this.starts[line] ?? this.text.length;
        let end =
            line + 1 < this.starts.length ? (this.starts[line + 1] ?? 0) - 1 : this.text.length;
        if (end > begin && this.text[end - 1] === "\r") {
            end -= 1;
        }
        return end - begin;
    }

    /// The text of line `line`, its break excluded.
    lineText(line: number): string {
        const begin = this.starts[line] ?? this.text.length;
        return this.text.slice(begin, begin + this.length(line));
    }

    position(offset: number): proto.Position {
        let low = 0;
        let high = this.starts.length - 1;
        while (low < high) {
            const mid = (low + high + 1) >> 1;
            if ((this.starts[mid] ?? 0) <= offset) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        return { line: low, character: offset - (this.starts[low] ?? 0) };
    }

    /// The offset of a reply position, or why it is no valid position.
    offset(position: proto.Position): number | string {
        if (position.line < 0 || position.line >= this.starts.length) {
            return `line ${position.line} past the last line`;
        }
        if (position.character < 0 || position.character > this.length(position.line)) {
            return `character ${position.character} past the end of line ${position.line}`;
        }
        const offset = (this.starts[position.line] ?? 0) + position.character;
        const unit = this.text.charCodeAt(offset);
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            return `character ${position.character} splits a surrogate pair`;
        }
        return offset;
    }
}

/// UTF-16 offsets, as the protocol counts them.
export interface Span {
    begin: number;
    end: number;
}

/// One line around a span, the span marked: `a ⟦b⟧ c`, a span over
/// several lines shown from its first line to its last.
export function snippet(lines: Lines, span: Span): string {
    const from = lines.position(span.begin);
    const to = lines.position(span.end);
    const first = lines.lineText(from.line);
    if (from.line === to.line) {
        return (
            first.slice(0, from.character) +
            "⟦" +
            first.slice(from.character, to.character) +
            "⟧" +
            first.slice(to.character)
        ).trim();
    }
    const last = lines.lineText(to.line);
    return (
        `${first.slice(0, from.character)}⟦${first.slice(from.character)}`.trim() +
        " … " +
        `${last.slice(0, to.character)}⟧${last.slice(to.character)}`.trim() +
        ` (${to.line - from.line + 1} lines)`
    );
}

/// Findings of one kind, each counted once per identity; the examples come
/// from different files where they can, a difference from clangd's
/// (`clice only`, `clangd only`) keeping its first few.
export class Groups<Example extends { at: string }> {
    readonly groups = new Map<string, { count: number; examples: Example[] }>();
    private readonly seen = new Set<string>();

    add(key: string, identity: string, example: () => Example): void {
        const limit = key.startsWith("clice only") || key.startsWith("clangd only") ? 4 : 12;
        if (this.seen.has(`${key}\u0000${identity}`)) {
            return;
        }
        this.seen.add(`${key}\u0000${identity}`);
        let group = this.groups.get(key);
        if (group === undefined) {
            group = { count: 0, examples: [] };
            this.groups.set(key, group);
        }
        group.count += 1;
        if (group.examples.length < limit) {
            const added = example();
            const file = added.at.replace(/(:\d+){1,2}$/, "");
            if (limit === 4 || !group.examples.some((e) => e.at.startsWith(file))) {
                group.examples.push(added);
            }
        }
    }

    sorted(): [string, { count: number; examples: Example[] }][] {
        return [...this.groups].sort((a, b) => b[1].count - a[1].count);
    }
}

export interface Server {
    kind: "clice" | "clangd";
    /// How the reports name it.
    name: string;
    client: CliceClient;
    /// Milliseconds per request, or per position a request carried.
    millis: number[];
    slowest: number;
}

export interface ServerOptions {
    name?: string | undefined;
    capabilities?: proto.ClientCapabilities | undefined;
    /// clice serves the files read-only.
    readonly?: boolean | undefined;
}

/// A server on the workspace `workspace`, whose compile_commands.json both
/// servers read; clice with its real defaults, one stateful worker and no
/// indexing.
export async function start(
    kind: "clice" | "clangd",
    binary: string,
    workspace: string,
    options: ServerOptions = {},
): Promise<Server> {
    const client = CliceClient.start(binary, {
        args:
            kind === "clice"
                ? ["serve"]
                : [`--compile-commands-dir=${workspace}`, "--background-index=false"],
        cwd: workspace,
    });
    await client.initialize(new Workspace(workspace), {
        testDefaults: false,
        capabilities: options.capabilities,
        initializationOptions:
            kind === "clice"
                ? {
                      rules: [{ compile_commands: [workspace] }],
                      project: {
                          enable_indexing: false,
                          stateful_worker_count: 1,
                          readonly: options.readonly === true ? "on" : "off",
                      },
                  }
                : {},
    });
    return { kind, name: options.name ?? kind, client, millis: [], slowest: 0 };
}

export async function stop(server: Server): Promise<void> {
    if (server.kind === "clice") {
        await server.client.shutdown();
        return;
    }
    try {
        await withTimeout(
            server.client.sendRequest(proto.ShutdownRequest.type),
            60_000,
            "shutdown",
        );
        await server.client.sendNotification(proto.ExitNotification.type);
    } catch {
        // Killed below.
    }
    server.client.disposed = true;
    // Hang detection: a server still running after `exit` is killed.
    setTimeout(() => server.client.child.kill("SIGKILL"), 5_000).unref();
    await server.client.exited;
}
