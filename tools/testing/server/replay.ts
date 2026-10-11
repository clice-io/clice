/// A session an editor recorded (record.ts), played back on a copy of its
/// project at the recorded pace: what the editor sent, as it sent it,
/// against the server's real defaults.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import type { Serve } from "./serve.ts";

/// One line of a trace: when the editor sent the message, in milliseconds
/// from the start, and the message as it was sent.
interface Line {
    ts: number;
    msg: string;
}

interface Message {
    id?: number | string;
    /// Unset on the editor's replies to the server's requests.
    method?: string;
    params?: unknown;
}

/// The trace's messages, its workspace root moved to `root` and the
/// server's client process made this one.
export function readTrace(file: string, root: string): { ts: number; message: Message }[] {
    const lines = fs
        .readFileSync(file, "utf8")
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => JSON.parse(line) as Line);
    const initialize = lines
        .map((line) => JSON.parse(line.msg) as Message)
        .find((message) => message.method === "initialize");
    const rootUri = (initialize?.params as { rootUri?: unknown } | undefined)?.rootUri;
    if (typeof rootUri !== "string") {
        throw new Error(`${file}: the trace opens with no initialize naming a rootUri`);
    }
    const from = { uri: rootUri, path: URI.parse(rootUri).path };
    const to = { uri: URI.file(root).toString(), path: root };
    const escape = (text: string): string => JSON.stringify(text).slice(1, -1);
    return lines.map(({ ts, msg }) => {
        const message = JSON.parse(
            msg
                .replaceAll(escape(from.uri), escape(to.uri))
                .replaceAll(escape(from.path), escape(to.path)),
        ) as Message;
        if (message.method === "initialize") {
            (message.params as { processId: number }).processId = process.pid;
        }
        return { ts, message };
    });
}

/// Play `file` to a server spawned without a handshake; every request must
/// be answered, with an error only for a request the editor cancelled or
/// whose document changed under it.
/// The trace's shutdown and exit are left to the test's teardown, which
/// gates the exit.
export async function replay(s: Serve, file: string): Promise<void> {
    const client = s.client;
    const messages = readTrace(file, s.workspace.root);
    const cancels = new Map<number | string, proto.CancellationTokenSource>();
    const answers: Promise<void>[] = [];
    const failures: string[] = [];
    /// The changes each document received so far, by URI.
    const changes = new Map<string, number>();
    const documentOf = (message: Message): string =>
        (message.params as { textDocument?: { uri?: string } } | undefined)?.textDocument?.uri ??
        "";
    let previous = messages[0]?.ts ?? 0;
    for (const { ts, message } of messages) {
        if (message.method === "shutdown" || message.method === "exit") {
            break;
        }
        await new Promise((resolve) => setTimeout(resolve, Math.max(0, ts - previous)));
        previous = ts;
        if (message.method === undefined) {
            // The editor's reply to a request of the server: the client
            // answers those itself.
            continue;
        }
        if (message.method === "$/cancelRequest") {
            cancels.get((message.params as { id: number | string }).id)?.cancel();
            continue;
        }
        if (message.id === undefined) {
            if (message.method === "textDocument/didChange") {
                const uri = documentOf(message);
                changes.set(uri, (changes.get(uri) ?? 0) + 1);
            }
            await client.sendNotification(message.method, message.params);
            continue;
        }
        const source = new proto.CancellationTokenSource();
        const id = message.id;
        const uri = documentOf(message);
        const seen = changes.get(uri) ?? 0;
        cancels.set(id, source);
        answers.push(
            client.sendRequest(message.method, message.params, source.token).then(
                () => undefined,
                (error: unknown) => {
                    const stale =
                        error instanceof proto.ResponseError &&
                        error.code === proto.LSPErrorCodes.ContentModified &&
                        (changes.get(uri) ?? 0) > seen;
                    if (!stale && !source.token.isCancellationRequested) {
                        failures.push(`${message.method} #${String(id)}: ${String(error)}`);
                    }
                },
            ),
        );
    }
    const exited = client.exited.then((code) => {
        throw new Error(`the server exited (code ${String(code)})`);
    });
    await Promise.race([Promise.all(answers), exited]);
    if (failures.length > 0) {
        throw new Error(`requests answered with an error:\n${failures.join("\n")}`);
    }
}
