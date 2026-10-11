/// LSP replies converted into the shapes `answers.ts` renders.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import type { CounterpartsResult } from "../../protocol/protocol.ts";
import type {
    Action,
    Call,
    CompletionItem,
    Counterparts,
    Diagnostic,
    Fold,
    Highlight,
    Hover,
    InlayHint,
    Item,
    Link,
    OutlineSymbol,
    Signatures,
    Token,
    WorkspaceSymbol,
} from "./answers.ts";
import type { TextEdit } from "./edits.ts";
import {
    displayPath,
    displayUri,
    outsidePath,
    resourceRoot,
    uriToPath,
    type Root,
} from "./paths.ts";
import { SourceText, type Location, type Named, type Range, type RenderScope } from "./position.ts";

/// What converting and rendering a reply needs to know about the
/// workspace the server answered for.
export class AnswerContext {
    readonly workspace: string;
    readonly roots: readonly Root[];
    private readonly read: (path: string) => string;
    private readonly cache = new Map<string, SourceText>();

    /// `read` returns the text requests see for a workspace path: the open
    /// buffer, else the disk. A file outside reads from the disk.
    constructor(workspace: string, read: (path: string) => string) {
        this.workspace = workspace;
        this.read = read;
        this.roots = [resourceRoot()];
    }

    path(uri: string): string {
        return displayUri(uri, this.workspace, this.roots);
    }

    pathOfFile(file: string): string {
        return displayPath(file, this.workspace, this.roots);
    }

    file(path: string): SourceText {
        const outside = outsidePath(path, this.roots);
        const text = outside === undefined ? this.read(path) : fs.readFileSync(outside, "utf8");
        const cached = this.cache.get(path);
        if (cached?.text === text) {
            return cached;
        }
        const source = new SourceText(path, text);
        this.cache.set(path, source);
        return source;
    }

    /// The file as requests see it, or undefined when it cannot be read.
    peek(path: string): SourceText | undefined {
        try {
            return this.file(path);
        } catch {
            return undefined;
        }
    }

    scope(query?: RenderScope["query"], names?: readonly Named[]): RenderScope {
        return { file: (path) => this.file(path), query, names };
    }

    location(location: proto.Location): Location {
        return { path: this.path(location.uri), range: location.range };
    }

    locations(
        reply: proto.Location | (proto.Location | proto.LocationLink)[] | null | undefined,
    ): Location[] {
        if (reply === null || reply === undefined) {
            return [];
        }
        return (Array.isArray(reply) ? reply : [reply]).map((entry) =>
            "uri" in entry
                ? this.location(entry)
                : { path: this.path(entry.targetUri), range: entry.targetSelectionRange },
        );
    }

    item(item: proto.CallHierarchyItem | proto.TypeHierarchyItem): Item {
        return {
            name: item.name,
            kind: enumName(proto.SymbolKind, item.kind),
            selection: { path: this.path(item.uri), range: item.selectionRange },
            detail: item.detail,
        };
    }

    incoming(calls: proto.CallHierarchyIncomingCall[] | null): Call[] {
        return (calls ?? []).map((call) => {
            const item = this.item(call.from);
            return {
                item,
                sites: call.fromRanges.map((range) => ({ path: item.selection.path, range })),
            };
        });
    }

    /// Outgoing call sites lie in the root's file, not the callee's.
    outgoing(
        root: proto.CallHierarchyItem,
        calls: proto.CallHierarchyOutgoingCall[] | null,
    ): Call[] {
        const path = this.path(root.uri);
        return (calls ?? []).map((call) => ({
            item: this.item(call.to),
            sites: call.fromRanges.map((range) => ({ path, range })),
        }));
    }

    highlights(path: string, reply: proto.DocumentHighlight[] | null): Highlight[] {
        return (reply ?? []).map((highlight) => ({
            location: { path, range: highlight.range },
            kind:
                highlight.kind === proto.DocumentHighlightKind.Read
                    ? "read"
                    : highlight.kind === proto.DocumentHighlightKind.Write
                      ? "write"
                      : "text",
        }));
    }

    hover(path: string, reply: proto.Hover | null): Hover | null {
        if (reply === null) {
            return null;
        }
        const parts = Array.isArray(reply.contents) ? reply.contents : [reply.contents];
        const markdown = parts
            .map((part) => (typeof part === "string" ? part : part.value))
            .join("\n");
        return {
            markdown,
            range: reply.range === undefined ? undefined : { path, range: reply.range },
        };
    }

    diagnostics(path: string, reply: readonly proto.Diagnostic[]): Diagnostic[] {
        return reply.map((diagnostic) => ({
            location: { path, range: diagnostic.range },
            severity:
                diagnostic.severity === proto.DiagnosticSeverity.Warning
                    ? "warning"
                    : diagnostic.severity === proto.DiagnosticSeverity.Information
                      ? "information"
                      : diagnostic.severity === proto.DiagnosticSeverity.Hint
                        ? "hint"
                        : "error",
            message: elideCrash(
                typeof diagnostic.message === "string"
                    ? diagnostic.message
                    : diagnostic.message.value,
            ),
            source: diagnostic.source,
            code: diagnostic.code === undefined ? undefined : String(diagnostic.code),
            related: (diagnostic.relatedInformation ?? []).map((info) => ({
                location: this.location(info.location),
                message: info.message,
            })),
            link: diagnostic.codeDescription?.href,
            tags: (diagnostic.tags ?? []).map((tag) => enumName(proto.DiagnosticTag, tag)),
        }));
    }

    inlayHints(reply: proto.InlayHint[] | null): InlayHint[] {
        return (reply ?? []).map((hint) => {
            const parts: proto.InlayHintLabelPart[] =
                typeof hint.label === "string" ? [{ value: hint.label }] : hint.label;
            return {
                position: hint.position,
                label: parts.map((part) => part.value).join(""),
                kind:
                    hint.kind === undefined
                        ? undefined
                        : (enumName(proto.InlayHintKind, hint.kind) as "Type" | "Parameter"),
                paddingLeft: hint.paddingLeft ?? false,
                paddingRight: hint.paddingRight ?? false,
                links: parts.flatMap((part) =>
                    part.location !== undefined
                        ? [{ value: part.value, location: this.location(part.location) }]
                        : [],
                ),
            };
        });
    }

    folds(path: string, reply: proto.FoldingRange[] | null): Fold[] {
        const source = this.file(path);
        return (reply ?? []).map((fold) => ({
            range: {
                start: { line: fold.startLine, character: fold.startCharacter ?? 0 },
                end: {
                    line: fold.endLine,
                    character: fold.endCharacter ?? source.line(fold.endLine).length,
                },
            },
            kind: fold.kind ?? "region",
            collapsedText: fold.collapsedText === "" ? undefined : fold.collapsedText,
        }));
    }

    outline(path: string, reply: proto.DocumentSymbol[] | null): OutlineSymbol[] {
        const convert = (symbol: proto.DocumentSymbol): OutlineSymbol => ({
            name: symbol.name,
            kind: enumName(proto.SymbolKind, symbol.kind),
            selection: { path, range: symbol.selectionRange },
            detail: symbol.detail,
            range: { path, range: symbol.range },
            tags: (symbol.tags ?? []).map((tag) => enumName(proto.SymbolTag, tag)),
            children: (symbol.children ?? []).map(convert),
        });
        return (reply ?? []).map(convert);
    }

    links(path: string, reply: proto.DocumentLink[] | null): Link[] {
        return (reply ?? []).map((link) => ({
            location: { path, range: link.range },
            target:
                link.target === undefined
                    ? "(unresolved)"
                    : this.pathOfFile(uriToPath(link.target)),
        }));
    }

    actions(reply: (proto.Command | proto.CodeAction)[] | null): Action[] {
        return (reply ?? []).map((item) => {
            if (!("kind" in item)) {
                throw new Error("clice replies with code actions of a kind, never commands");
            }
            return {
                kind: item.kind,
                title: item.title,
                edits: this.workspaceEdit(item.edit ?? null),
                host: [],
            };
        });
    }

    workspaceSymbols(
        reply: proto.SymbolInformation[] | proto.WorkspaceSymbol[] | null,
    ): WorkspaceSymbol[] {
        return (reply ?? []).map((symbol) => {
            if (!("range" in symbol.location)) {
                throw new Error("clice replies with symbols at full locations");
            }
            return {
                name: symbol.name,
                kind: enumName(proto.SymbolKind, symbol.kind),
                selection: this.location(symbol.location),
                container: symbol.containerName,
            };
        });
    }

    counterparts(reply: CounterpartsResult): Counterparts {
        return {
            preferred: reply.preferred === null ? undefined : this.path(reply.preferred),
            candidates: reply.candidates.map((candidate) => ({
                path: this.path(candidate.uri),
                reasons: candidate.reasons,
            })),
        };
    }

    workspaceEdit(edit: proto.WorkspaceEdit | null): Map<string, TextEdit[]> {
        const out = new Map<string, TextEdit[]>();
        const add = (
            uri: string,
            edits: readonly (proto.TextEdit | proto.AnnotatedTextEdit)[],
        ): void => {
            const path = this.path(uri);
            out.set(path, [...(out.get(path) ?? []), ...edits]);
        };
        for (const change of edit?.documentChanges ?? []) {
            if (!("textDocument" in change)) {
                throw new Error(
                    `workspace edits create, rename or delete no file here: ${change.kind}`,
                );
            }
            add(
                change.textDocument.uri,
                change.edits.filter((item): item is proto.TextEdit => "newText" in item),
            );
        }
        for (const [uri, edits] of Object.entries(edit?.changes ?? {})) {
            add(uri, edits);
        }
        return out;
    }
}

/// A worker's way of dying differs by platform (a signal, a Windows
/// exception): the text names it `${CRASH}`. The pool's crash report
/// gives the bare number, a signal or a Windows exit code, before the
/// crash streak.
export function elideCrash(text: string): string {
    return text.replace(
        /killed by signal \d+ \(\w+\)|terminated by (?:exception )?0x[0-9A-Fa-f]+(?: \([^)]*\))?|(?:killed by signal|exited with code) \d+(?= \(crash streak)/g,
        "${CRASH}",
    );
}

export function enumName(table: Record<string, unknown>, value: number): string {
    for (const [name, entry] of Object.entries(table)) {
        if (entry === value) {
            return name;
        }
    }
    return String(value);
}

/// `path` is the document completed in, where the items' edits apply.
export function completionItems(
    reply: proto.CompletionItem[] | proto.CompletionList | null,
    path = "",
): CompletionItem[] {
    const items = reply === null ? [] : Array.isArray(reply) ? reply : reply.items;
    return items.map((item) => {
        const edit = item.textEdit;
        const split = edit !== undefined && "insert" in edit;
        return {
            label: item.label + (item.labelDetails?.detail ?? ""),
            kind: item.kind === undefined ? "None" : enumName(proto.CompletionItemKind, item.kind),
            detail: item.detail ?? item.labelDetails?.description,
            deprecated: item.tags?.includes(proto.CompletionItemTag.Deprecated) ?? false,
            filter: item.filterText,
            insert: edit?.newText ?? item.insertText,
            snippet: item.insertTextFormat === proto.InsertTextFormat.Snippet,
            replace:
                edit === undefined
                    ? undefined
                    : { path, range: "range" in edit ? edit.range : edit.replace },
            insertRange:
                split && JSON.stringify(edit.insert) !== JSON.stringify(edit.replace)
                    ? { path, range: edit.insert }
                    : undefined,
            sortText: item.sortText ?? "",
        };
    });
}

export function signatures(reply: proto.SignatureHelp | null): Signatures {
    if (reply === null) {
        return { signatures: [], active: 0 };
    }
    const active = reply.activeSignature ?? 0;
    return {
        active,
        signatures: reply.signatures.map((signature, index) => {
            const parameterIndex =
                signature.activeParameter ?? (index === active ? reply.activeParameter : undefined);
            const parameter =
                parameterIndex === undefined || parameterIndex === null
                    ? undefined
                    : signature.parameters?.[parameterIndex];
            let span: { begin: number; end: number } | undefined;
            if (parameter !== undefined) {
                if (typeof parameter.label === "string") {
                    const begin = signature.label.indexOf(parameter.label);
                    span = begin < 0 ? undefined : { begin, end: begin + parameter.label.length };
                } else {
                    span = { begin: parameter.label[0], end: parameter.label[1] };
                }
            }
            return { label: signature.label, active: span };
        }),
    };
}

/// A selection range reply's chain around one place, innermost first.
export function selection(reply: proto.SelectionRange | undefined): Range[] {
    const ranges: Range[] = [];
    for (let step = reply; step !== undefined; step = step.parent) {
        ranges.push(step.range);
    }
    return ranges;
}

/// Semantic tokens decoded through the server's legend.
export function tokens(
    reply: proto.SemanticTokens | null,
    legend: proto.SemanticTokensLegend,
): Token[] {
    const data = reply?.data ?? [];
    const out: Token[] = [];
    let line = 0;
    let character = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        const [deltaLine, deltaStart, length, type, modifiers] = data.slice(i, i + 5) as [
            number,
            number,
            number,
            number,
            number,
        ];
        line += deltaLine;
        character = deltaLine === 0 ? character + deltaStart : deltaStart;
        out.push({
            line,
            character,
            length,
            type: legend.tokenTypes[type] ?? String(type),
            modifiers: legend.tokenModifiers.filter((_name, bit) => modifiers & (1 << bit)),
        });
    }
    return out;
}
