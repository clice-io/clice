import * as vscode from "vscode";
import type { Middleware } from "vscode-languageclient/node";
import type { ClientHandle } from "../client";

/// A request in the header name starting at `start` of `line`, with the
/// name typed so far, and whether its answer was empty (undefined until it
/// comes).
interface Request {
    uri: string;
    line: number;
    start: number;
    name: string;
    empty?: boolean;
}

/// The header name typed before `position`, with where it starts: in an
/// `#include`, `#include_next` or `#import`, or in a `__has_include`.
function headerName(
    document: vscode.TextDocument,
    position: vscode.Position,
): { start: number; name: string } | undefined {
    const before = document.lineAt(position.line).text.slice(0, position.character);
    const name =
        /(?:^\s*#\s*(?:include|include_next|import)\s*|__has_include(?:_next)?\s*\(\s*)[<"]([^<>"]*)$/.exec(
            before,
        )?.[1];
    return name === undefined ? undefined : { start: position.character - name.length, name };
}

/// VS Code draws a file item's icon by its label, or by its `detail`, read
/// as a file name, and an icon theme tells a header by its extension: an
/// item the server takes for a header (its `data` is `"header"`) without
/// one (`vector`) takes a `detail` of `.h`, which shows in the details only
/// once the label is one of its own.
function markHeader(item: vscode.CompletionItem) {
    const label = typeof item.label === "string" ? item.label : item.label.label;
    if ((item as { data?: unknown }).data !== "header" || label.includes(".")) {
        return;
    }
    item.detail = ".h";
    if (typeof item.label === "string") {
        item.label = { label };
    }
}

/// VS Code reads a header name as a string, where typing asks for no
/// completion: only a trigger character, or a keystroke after an incomplete
/// list, asks again. Once the name typed matches nothing (`<bec`) the
/// suggestions close, and typing on in the name (`<vec`, after deleting
/// back) would ask nothing. A character typed in a header name asks again
/// unless VS Code just asked for the name itself, or the name goes on from
/// one that matched nothing.
export function registerIncludeCompletion(
    client: () => ClientHandle | undefined,
    served: vscode.DocumentSelector,
    context: vscode.ExtensionContext,
): NonNullable<Middleware["provideCompletionItem"]> {
    let last: Request | undefined;

    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument((event) => {
            const [change] = event.contentChanges;
            if (
                event.contentChanges.length !== 1 ||
                change.text.length !== 1 ||
                event.reason !== undefined ||
                vscode.window.activeTextEditor?.document !== event.document ||
                vscode.languages.match(served, event.document) === 0
            ) {
                return;
            }
            const triggers =
                client()?.current.initializeResult?.capabilities.completionProvider
                    ?.triggerCharacters ?? [];
            const typed = change.range.start;
            const typing = headerName(event.document, typed.translate(0, 1));
            if (triggers.includes(change.text) || !typing) {
                return;
            }
            const here =
                last?.uri === event.document.uri.toString() &&
                last.line === typed.line &&
                last.start === typing.start
                    ? last
                    : undefined;
            const asked =
                here !== undefined &&
                here.start + here.name.length === typed.character &&
                here.empty !== true;
            const hopeless = here?.empty === true && typing.name.startsWith(here.name);
            if (!asked && !hopeless) {
                void vscode.commands.executeCommand("editor.action.triggerSuggest", {
                    auto: true,
                });
            }
        }),
    );

    return async (document, position, completionContext, token, next) => {
        const typing = headerName(document, position);
        if (!typing) {
            return next(document, position, completionContext, token);
        }
        const request: Request = { uri: document.uri.toString(), line: position.line, ...typing };
        last = request;
        const answer = await next(document, position, completionContext, token);
        // A request typing overtook tells nothing of the name: the next
        // keystroke asks again.
        if (token.isCancellationRequested) {
            if (last === request) {
                last = undefined;
            }
            return answer;
        }
        const items = Array.isArray(answer) ? answer : (answer?.items ?? []);
        request.empty = items.length === 0;
        for (const item of items) {
            markHeader(item);
        }
        // A complete list VS Code filters itself, and may close without
        // asking: incomplete, every keystroke in the name asks.
        return new vscode.CompletionList(items, true);
    };
}
