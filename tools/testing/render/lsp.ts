/// LSP replies converted into the shapes `answers.ts` renders.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import type { CounterpartsResult } from "../../protocol/protocol.ts";
import type {
    Action,
    Call,
    Completion,
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
import {
    SourceText,
    type Location,
    type Named,
    type Position,
    type PositionEncoding,
    type Range,
    type RenderScope,
} from "./position.ts";

/// What converting and rendering a reply needs to know about the
/// workspace the server answered for.
export class AnswerContext {
    readonly workspace: string;
    readonly roots: readonly Root[];
    private readonly read: (path: string) => string;
    private readonly encoding: PositionEncoding;
    private readonly cache = new Map<string, SourceText>();

    /// `read` returns the text requests see for a workspace path: the open
    /// buffer, else the disk. A file outside reads from the disk. Replies
    /// count positions in `encoding`, the answers converted from them in
    /// UTF-16.
    constructor(workspace: string, read: (path: string) => string, encoding: PositionEncoding) {
        this.workspace = workspace;
        this.read = read;
        this.encoding = encoding;
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

    /// A reply's range in `path` as answers count it.
    range(path: string, range: Range): Range {
        return this.encoding === "utf-16"
            ? range
            : this.file(path).decodeRange(range, this.encoding);
    }

    /// A range of `path` as the server counts it.
    encodedRange(path: string, range: Range): Range {
        return this.encoding === "utf-16"
            ? range
            : this.file(path).encodeRange(range, this.encoding);
    }

    private position(path: string, position: Position): Position {
        return this.range(path, { start: position, end: position }).start;
    }

    private placed(path: string, range: Range): Location {
        return { path, range: this.range(path, range) };
    }

    location(location: proto.Location): Location {
        return this.placed(this.path(location.uri), location.range);
    }

    edits(path: string, edits: readonly proto.TextEdit[]): TextEdit[] {
        return edits.map((edit) => ({
            range: this.range(path, edit.range),
            newText: edit.newText,
        }));
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
                : this.placed(this.path(entry.targetUri), entry.targetSelectionRange),
        );
    }

    item(item: proto.CallHierarchyItem | proto.TypeHierarchyItem): Item {
        return {
            name: item.name,
            kind: enumName(proto.SymbolKind, item.kind),
            selection: this.placed(this.path(item.uri), item.selectionRange),
            detail: item.detail,
        };
    }

    incoming(calls: proto.CallHierarchyIncomingCall[] | null): Call[] {
        return (calls ?? []).map((call) => {
            const item = this.item(call.from);
            return {
                item,
                sites: call.fromRanges.map((range) => this.placed(item.selection.path, range)),
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
            sites: call.fromRanges.map((range) => this.placed(path, range)),
        }));
    }

    highlights(path: string, reply: proto.DocumentHighlight[] | null): Highlight[] {
        return (reply ?? []).map((highlight) => ({
            location: this.placed(path, highlight.range),
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
        if (proto.MarkupContent.is(reply.contents)) {
            return {
                kind: reply.contents.kind,
                text: reply.contents.value,
                range: reply.range === undefined ? undefined : this.placed(path, reply.range),
            };
        }
        // Marked strings, which LSP deprecates, are markdown.
        const parts = Array.isArray(reply.contents) ? reply.contents : [reply.contents];
        return {
            kind: "markdown",
            text: parts.map((part) => (typeof part === "string" ? part : part.value)).join("\n"),
            range: reply.range === undefined ? undefined : this.placed(path, reply.range),
        };
    }

    diagnostics(path: string, reply: readonly proto.Diagnostic[]): Diagnostic[] {
        return reply.map((diagnostic) => ({
            location: this.placed(path, diagnostic.range),
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

    inlayHints(path: string, reply: proto.InlayHint[] | null): InlayHint[] {
        return (reply ?? []).map((hint) => {
            const parts: proto.InlayHintLabelPart[] =
                typeof hint.label === "string" ? [{ value: hint.label }] : hint.label;
            return {
                position: this.position(path, hint.position),
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
                start: this.position(path, {
                    line: fold.startLine,
                    character: fold.startCharacter ?? 0,
                }),
                end:
                    fold.endCharacter === undefined
                        ? { line: fold.endLine, character: source.line(fold.endLine).length }
                        : this.position(path, { line: fold.endLine, character: fold.endCharacter }),
            },
            kind: fold.kind,
            collapsedText: fold.collapsedText === "" ? undefined : fold.collapsedText,
        }));
    }

    /// A tree, or a flat reply to a client that takes no tree: each symbol
    /// at its whole extent, beside the name of the one it nests in.
    outline(
        path: string,
        reply: proto.DocumentSymbol[] | proto.SymbolInformation[] | null,
    ): OutlineSymbol[] {
        const convert = (symbol: proto.DocumentSymbol): OutlineSymbol => ({
            name: symbol.name,
            kind: enumName(proto.SymbolKind, symbol.kind),
            selection: this.placed(path, symbol.selectionRange),
            detail: symbol.detail,
            range: this.placed(path, symbol.range),
            tags: (symbol.tags ?? []).map((tag) => enumName(proto.SymbolTag, tag)),
            children: (symbol.children ?? []).map(convert),
        });
        return (reply ?? []).map((symbol) =>
            "selectionRange" in symbol
                ? convert(symbol)
                : {
                      name: symbol.name,
                      kind: enumName(proto.SymbolKind, symbol.kind),
                      selection: this.location(symbol.location),
                      tags: (symbol.tags ?? []).map((tag) => enumName(proto.SymbolTag, tag)),
                      container: symbol.containerName,
                      children: [],
                  },
        );
    }

    links(path: string, reply: proto.DocumentLink[] | null): Link[] {
        return (reply ?? []).map((link) => ({
            location: this.placed(path, link.range),
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
            out.set(path, [...(out.get(path) ?? []), ...this.edits(path, edits)]);
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

    /// `path` is the document completed in, where the items' edits apply.
    completion(
        path: string,
        reply: proto.CompletionItem[] | proto.CompletionList | null,
    ): Completion {
        const items = reply === null ? [] : Array.isArray(reply) ? reply : reply.items;
        const incomplete = reply !== null && !Array.isArray(reply) && reply.isIncomplete;
        return {
            incomplete,
            items: items.map((item) => {
                const edit = item.textEdit;
                const split = edit !== undefined && "insert" in edit;
                return {
                    label: item.label + (item.labelDetails?.detail ?? ""),
                    kind:
                        item.kind === undefined
                            ? "None"
                            : enumName(proto.CompletionItemKind, item.kind),
                    detail: item.detail ?? item.labelDetails?.description,
                    deprecated: item.tags?.includes(proto.CompletionItemTag.Deprecated) ?? false,
                    filter: item.filterText,
                    insert: edit?.newText ?? item.insertText,
                    snippet: item.insertTextFormat === proto.InsertTextFormat.Snippet,
                    replace:
                        edit === undefined
                            ? undefined
                            : this.placed(path, "range" in edit ? edit.range : edit.replace),
                    insertRange:
                        split && JSON.stringify(edit.insert) !== JSON.stringify(edit.replace)
                            ? this.placed(path, edit.insert)
                            : undefined,
                    sortText: item.sortText ?? "",
                    documentation:
                        typeof item.documentation === "string"
                            ? item.documentation
                            : item.documentation?.value,
                    edits: item.additionalTextEdits?.map((extra) => ({
                        at: this.placed(path, extra.range),
                        text: extra.newText,
                    })),
                };
            }),
        };
    }

    /// A selection range reply's chain around one place, innermost first.
    selection(path: string, reply: proto.SelectionRange | undefined): Range[] {
        const ranges: Range[] = [];
        for (let step = reply; step !== undefined; step = step.parent) {
            ranges.push(this.range(path, step.range));
        }
        return ranges;
    }

    /// The semantic tokens of `path`, decoded through the server's legend.
    tokens(
        path: string,
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
            const { start, end } = this.range(path, {
                start: { line, character },
                end: { line, character: character + length },
            });
            out.push({
                line,
                character: start.character,
                length: end.character - start.character,
                type: legend.tokenTypes[type] ?? String(type),
                modifiers: legend.tokenModifiers.filter((_name, bit) => modifiers & (1 << bit)),
            });
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
        /killed by signal \d+ \(\w+\)|terminated by (?:exception )?0x[0-9A-Fa-f]+(?: \([^)]*\))?|(?:killed by signal|exited with code) -?\d+(?= \(crash streak)/g,
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

/// A parameter's label offset into its signature's label, counted in
/// `encoding`, as a string index.
function labelIndex(label: string, offset: number, encoding: PositionEncoding): number {
    return encoding === "utf-16"
        ? offset
        : Buffer.from(label, "utf8").subarray(0, offset).toString("utf8").length;
}

/// The parameters' label offsets count in `encoding`.
export function signatures(
    reply: proto.SignatureHelp | null,
    encoding: PositionEncoding,
): Signatures {
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
                    const [begin, end] = parameter.label;
                    span = {
                        begin: labelIndex(signature.label, begin, encoding),
                        end: labelIndex(signature.label, end, encoding),
                    };
                }
            }
            const text = (value: string | proto.MarkupContent | undefined) =>
                typeof value === "object" ? value.value : value;
            const documentation = text(signature.documentation);
            const parameterDocumentation = text(parameter?.documentation);
            return {
                label: signature.label,
                active: span,
                ...(documentation === undefined ? {} : { documentation }),
                ...(parameterDocumentation === undefined ? {} : { parameterDocumentation }),
            };
        }),
    };
}
