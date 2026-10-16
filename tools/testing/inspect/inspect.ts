/// `clice inspect` as a client: one source file at a time, over a copy of
/// the case, its databases giving the commands. Each run compiles the file
/// once, so a feature's first request at a marker asks at every marker of
/// the file in the same run and the rest read its answers.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import type * as proto from "vscode-languageserver-protocol";
import { runProcess } from "../../client/process.ts";
import { cliceExecutable } from "../../client/session.ts";
import { serverEnv } from "../../process_gate.ts";
import { locate, type Anchor, type MarkerAnchor } from "../cases/anchors.ts";
import { recordConfig, recordInvocation } from "../coverage/record.ts";
import type {
    Action,
    Call,
    Completion,
    Counterparts,
    Fold,
    Highlight,
    Hover,
    InlayHint,
    Item,
    Link,
    OutlineSymbol,
    Refused,
    Renameable,
    Renamed,
    Popup,
    Signatures,
    Token,
    WorkspaceSymbol,
} from "../render/answers.ts";
import { Client, type LocationRequest, type Place } from "../render/client.ts";
import { applyEdits, type TextEdit } from "../render/edits.ts";
import { AnswerContext, signatures } from "../render/lsp.ts";
import { withoutBom, type Location, type Range, type SourceText } from "../render/position.ts";

/// One file entry of the output: a document feature fills `result`, a
/// marker feature `markers` by the names --markers gave (null for a place
/// with no answer).
interface Entry {
    result?: unknown;
    markers?: Record<string, unknown> | null;
    error?: string | null;
    diagnostics?: string[] | null;
}

interface Output {
    feature: string;
    files: Record<string, Entry>;
}

interface Places {
    points?: Record<string, number>;
    ranges?: Record<string, { begin: number; end: number }>;
}

interface RawRange {
    begin: number;
    end: number;
}

interface RawHover {
    range: RawRange | null;
    kind: "markdown" | "plaintext";
    contents: string;
}

/// The protocol item, in snake_case with enums by name.
interface RawCompletionItem {
    label: string;
    kind?: string | null;
    detail?: string | null;
    sort_text?: string | null;
    filter_text?: string | null;
    label_details?: { detail?: string | null; description?: string | null } | null;
    tags?: string[] | null;
    insert_text?: string | null;
    insert_text_format?: string | null;
    documentation?: string | { kind: string; value: string } | null;
    /// Ranges already in LSP positions.
    text_edit?:
        | { range: proto.Range; new_text: string }
        | { insert: proto.Range; replace: proto.Range; new_text: string }
        | null;
    additional_text_edits?: { range: proto.Range; new_text: string }[] | null;
}

interface RawCompletionList {
    is_incomplete: boolean;
    items: RawCompletionItem[];
}

interface RawSignatureHelp {
    signatures?:
        | {
              label: string;
              documentation?: string | null;
              parameters?:
                  | { label: string | [number, number]; documentation?: string | null }[]
                  | null;
              active_parameter?: number | null;
          }[]
        | null;
    active_signature?: number | null;
    active_parameter?: number | null;
}

interface RawHighlight {
    range: RawRange;
    kind: "Text" | "Read" | "Write";
}

interface RawCodeAction {
    title: string;
    kind: string;
    /// Replacements of the requested file.
    edits: { range: RawRange; text: string }[];
    host_definitions?: string[] | null;
}

interface RawToken {
    range: RawRange;
    kind: string;
    modifiers: number;
}

interface RawInlayHint {
    offset: number;
    kind: string;
    label: { value: string; location?: { path: string; range: proto.Range } | null }[];
    padding_left: boolean;
    padding_right: boolean;
}

interface RawFold {
    range: RawRange;
    kind?: string | null;
    collapsed_text: string;
}

interface RawSymbol {
    name: string;
    detail: string;
    kind: string;
    range: RawRange;
    selection_range: RawRange;
    deprecated: boolean;
    children: RawSymbol[];
}

interface RawLink {
    range: RawRange;
    /// The target's absolute path.
    target: string;
}

/// Twin of SymbolModifiers::Kind (src/semantic/symbol.cppm), in bit order:
/// the server names them through its legend.
const SYMBOL_MODIFIERS = [
    "Declaration",
    "Definition",
    "Const",
    "Overloaded",
    "Typed",
    "Templated",
    "Deprecated",
    "Deduced",
    "Readonly",
    "Static",
    "Abstract",
    "Virtual",
    "DependentName",
    "DefaultLibrary",
    "UsedAsMutableReference",
    "UsedAsMutablePointer",
    "ConstructorOrDestructor",
    "UserDefined",
    "FunctionScope",
    "ClassScope",
    "FileScope",
    "GlobalScope",
    "Inactive",
];

/// Twin of to_protocol_symbol_kind (src/feature/document_symbols.cpp);
/// anything unlisted is Variable there too.
const LSP_SYMBOL_KIND: Record<string, string> = {
    Module: "Module",
    Namespace: "Namespace",
    Class: "Class",
    Struct: "Struct",
    Union: "Class",
    Enum: "Enum",
    Type: "Class",
    Concept: "TypeParameter",
    Field: "Field",
    EnumMember: "EnumMember",
    Function: "Function",
    Method: "Method",
    Macro: "Constant",
    Directive: "File",
    Comment: "String",
    Character: "String",
    String: "String",
    Header: "String",
    Number: "Number",
    Operator: "Operator",
    Paren: "Operator",
    Bracket: "Operator",
    Brace: "Operator",
    Angle: "Operator",
};

/// Twin of the HintCategory -> protocol::InlayHintKind switch in
/// src/feature/inlay_hints.cpp.
const LSP_INLAY_KIND: Record<string, "Type" | "Parameter"> = {
    Parameter: "Parameter",
    DefaultArgument: "Parameter",
    Type: "Type",
    Designator: "Type",
    BlockEnd: "Type",
};

/// C++ enum names are PascalCase; the server's legend lowers the first
/// letter.
function lowerFirst(name: string): string {
    return name.slice(0, 1).toLowerCase() + name.slice(1);
}

function byteOf(text: string, offset: number): number {
    return Buffer.byteLength(text.slice(0, offset), "utf8");
}

/// A module import, which a file compiled alone cannot resolve; one in an
/// inactive branch counts too.
const MODULE_IMPORT = /^[ \t]*(?:export[ \t]+)?import(?:[ \t]+\w|[ \t]*(?:[<"]|:(?!:)))/m;

function rangeKey({ begin, end }: RawRange): string {
    return `${String(begin)}-${String(end)}`;
}

export class Inspect extends Client {
    private readonly root: string;
    private readonly sources: readonly string[];
    private readonly importers: ReadonlySet<string>;
    private readonly config: Record<string, unknown> | undefined;
    private readonly scratch = fs.mkdtempSync(path.join(os.tmpdir(), "clice-markers-"));
    private readonly batches = new Map<string, Promise<Entry>>();
    private readonly documents = new Map<string, Promise<Entry>>();
    /// The errors each file compiled with, as inspected.
    private readonly compiled = new Map<string, string[]>();
    private runs = 0;

    /// `sources` are the copy's source files: those importing a module
    /// cannot compile alone and have no answer here.
    constructor(
        root: string,
        marks: readonly MarkerAnchor[],
        options: { sources: readonly string[]; config?: Record<string, unknown> | undefined },
    ) {
        super(marks);
        this.root = root;
        this.importers = new Set(
            options.sources.filter((file) => MODULE_IMPORT.test(this.text(file))),
        );
        this.sources = options.sources.filter((file) => !this.importers.has(file));
        this.config = options.config;
        if (options.config !== undefined) {
            recordConfig(options.config);
        }
    }

    protected context(): AnswerContext {
        return new AnswerContext(this.root, (file) => this.text(file));
    }

    /// The errors of every source but an importer and of every file
    /// inspected, each compiled alone; a source no request inspected
    /// compiles now.
    async compileErrors(): Promise<string[]> {
        for (const file of this.sources) {
            if (!this.compiled.has(file)) {
                this.record(file, await this.run("folding-range", file));
            }
        }
        return [...this.compiled.values()].flat();
    }

    close(): void {
        fs.rmSync(this.scratch, { recursive: true, force: true });
    }

    protected async fetchHover(at: Anchor): Promise<Hover | null> {
        const raw = (await this.atPoint("hover", at)) as RawHover | null;
        if (raw === null) {
            return null;
        }
        return {
            kind: raw.kind,
            text: raw.contents,
            range: raw.range === null ? undefined : this.location(at.path, raw.range),
        };
    }

    protected async fetchCompletion(at: Anchor, trigger: string | undefined): Promise<Completion> {
        if (trigger !== undefined) {
            return this.cannot("completion triggered by a character", at);
        }
        const raw = (await this.atPoint("code-completion", at)) as RawCompletionList;
        return {
            incomplete: raw.is_incomplete,
            items: raw.items.map((item) => {
                const edit = item.text_edit ?? undefined;
                const split = edit !== undefined && "insert" in edit;
                return {
                    label: item.label + (item.label_details?.detail ?? ""),
                    kind: item.kind ?? "None",
                    detail: item.detail ?? item.label_details?.description ?? undefined,
                    deprecated: item.tags?.includes("Deprecated") ?? false,
                    filter: item.filter_text ?? undefined,
                    insert: edit?.new_text ?? item.insert_text ?? undefined,
                    snippet: item.insert_text_format === "Snippet",
                    replace:
                        edit === undefined
                            ? undefined
                            : { path: at.path, range: "range" in edit ? edit.range : edit.replace },
                    insertRange:
                        split && JSON.stringify(edit.insert) !== JSON.stringify(edit.replace)
                            ? { path: at.path, range: edit.insert }
                            : undefined,
                    sortText: item.sort_text ?? "",
                    documentation:
                        typeof item.documentation === "string"
                            ? item.documentation
                            : (item.documentation?.value ?? undefined),
                    edits: item.additional_text_edits?.map((extra) => ({
                        at: { path: at.path, range: extra.range },
                        text: extra.new_text,
                    })),
                };
            }),
        };
    }

    protected async fetchSignatureHelp(at: Anchor, popup: Popup | undefined): Promise<Signatures> {
        if (popup !== undefined) {
            return this.cannot("signature help asked again with its popup shown", at);
        }
        const raw = (await this.atPoint("signature-help", at)) as RawSignatureHelp | null;
        if (raw === null) {
            return signatures(null);
        }
        return signatures({
            signatures: (raw.signatures ?? []).map((signature) => ({
                label: signature.label,
                ...(signature.documentation == null
                    ? {}
                    : { documentation: signature.documentation }),
                parameters: (signature.parameters ?? []).map((parameter) => ({
                    label: parameter.label,
                    ...(parameter.documentation == null
                        ? {}
                        : { documentation: parameter.documentation }),
                })),
                ...(signature.active_parameter == null
                    ? {}
                    : { activeParameter: signature.active_parameter }),
            })),
            ...(raw.active_signature == null ? {} : { activeSignature: raw.active_signature }),
            ...(raw.active_parameter == null ? {} : { activeParameter: raw.active_parameter }),
        });
    }

    protected async fetchHighlights(at: Anchor): Promise<Highlight[]> {
        const raws = ((await this.atPoint("document-highlight", at)) ?? []) as RawHighlight[];
        return raws.map((raw) => ({
            location: this.location(at.path, raw.range),
            kind: raw.kind === "Read" ? "read" : raw.kind === "Write" ? "write" : "text",
        }));
    }

    protected async fetchSelectionRange(at: Anchor): Promise<Range[]> {
        const raws = ((await this.atPoint("selection-range", at)) ?? []) as RawRange[];
        return raws.map((raw) => this.range(at.path, raw));
    }

    protected async fetchCodeActions(
        file: string,
        selection: Range,
        only: readonly string[] | undefined,
    ): Promise<Action[]> {
        if (only !== undefined) {
            return this.cannot("code actions of some kinds", file);
        }
        const raws = ((await this.overRange("code-action", file, selection)) ??
            []) as RawCodeAction[];
        return raws.map((raw) => {
            const edits = raw.edits.map((edit) => ({
                range: this.range(file, edit.range),
                newText: edit.text,
            }));
            return {
                kind: raw.kind,
                title: raw.title,
                edits: new Map(edits.length === 0 ? [] : [[file, edits]]),
                host: raw.host_definitions ?? [],
            };
        });
    }

    protected async compileWith(
        file: string,
        edits: ReadonlyMap<string, readonly TextEdit[]>,
    ): Promise<string[]> {
        const context = this.context();
        const originals = new Map([...edits.keys()].map((rel) => [rel, this.text(rel)]));
        for (const [rel, list] of edits) {
            fs.writeFileSync(path.join(this.root, rel), applyEdits(context.file(rel), list));
        }
        try {
            return [...((await this.run("folding-range", file)).diagnostics ?? [])].sort();
        } finally {
            for (const [rel, text] of originals) {
                fs.writeFileSync(path.join(this.root, rel), text);
            }
        }
    }

    protected fetchLocations(request: LocationRequest, at: Anchor): Promise<Location[]> {
        return this.cannot(request, at);
    }

    protected fetchHierarchy(hierarchy: "call" | "type", at: Anchor): Promise<Item[]> {
        return this.cannot(`${hierarchy} hierarchy`, at);
    }

    protected fetchCalls(direction: "incoming" | "outgoing", at: Anchor): Promise<Call[]> {
        return this.cannot(`${direction} calls`, at);
    }

    protected fetchTypes(direction: "supertypes" | "subtypes", at: Anchor): Promise<Item[]> {
        return this.cannot(direction, at);
    }

    protected async fetchTokens(file: string): Promise<Token[]> {
        const source = this.context().file(file);
        const size = Buffer.byteLength(source.text, "utf8");
        return ((await this.document("semantic-tokens", file)) as RawToken[])
            .filter((raw) => raw.range.begin < raw.range.end && raw.range.end <= size)
            .flatMap((raw) => this.pieces(source, raw));
    }

    protected async fetchInlayHints(file: string, range: Range): Promise<InlayHint[]> {
        const context = this.context();
        const source = context.file(file);
        const raws = ((await this.overRange("inlay-hint", file, range)) ?? []) as RawInlayHint[];
        return raws.map((raw) => {
            const kind = LSP_INLAY_KIND[raw.kind];
            if (kind === undefined) {
                throw new Error(`unmapped inlay hint kind '${raw.kind}'; extend LSP_INLAY_KIND`);
            }
            return {
                position: source.positionAtByte(raw.offset),
                label: raw.label.map((part) => part.value).join(""),
                kind,
                paddingLeft: raw.padding_left,
                paddingRight: raw.padding_right,
                links: raw.label.flatMap((part) =>
                    part.location == null
                        ? []
                        : [
                              {
                                  value: part.value,
                                  location: {
                                      path: context.pathOfFile(part.location.path),
                                      range: part.location.range,
                                  },
                              },
                          ],
                ),
            };
        });
    }

    protected async fetchFolds(file: string): Promise<Fold[]> {
        return ((await this.document("folding-range", file)) as RawFold[]).map((raw) => ({
            range: this.range(file, raw.range),
            kind: raw.kind ?? undefined,
            collapsedText: raw.collapsed_text === "" ? undefined : raw.collapsed_text,
        }));
    }

    protected async fetchSymbols(file: string): Promise<OutlineSymbol[]> {
        const convert = (raw: RawSymbol): OutlineSymbol => ({
            name: raw.name,
            kind: LSP_SYMBOL_KIND[raw.kind] ?? "Variable",
            selection: this.location(file, raw.selection_range),
            detail: raw.detail,
            range: this.location(file, raw.range),
            tags: raw.deprecated ? ["Deprecated"] : [],
            children: raw.children.map(convert),
        });
        return ((await this.document("document-symbols", file)) as RawSymbol[]).map(convert);
    }

    protected async fetchLinks(file: string): Promise<Link[]> {
        const context = this.context();
        return ((await this.document("document-links", file)) as RawLink[]).map((raw) => ({
            location: this.location(file, raw.range),
            target: context.pathOfFile(raw.target),
        }));
    }

    protected fetchCounterparts(file: string): Promise<Counterparts> {
        return this.cannot("counterparts", file);
    }

    protected fetchWorkspaceSymbols(query: string): Promise<WorkspaceSymbol[]> {
        return this.cannot("workspace symbols", query);
    }

    protected fetchFormatting(file: string): Promise<TextEdit[] | null> {
        return this.cannot("formatting", file);
    }

    protected fetchPrepareRename(at: Anchor): Promise<Renameable | Refused | null> {
        return this.cannot("prepareRename", at);
    }

    protected fetchRename(at: Anchor): Promise<Renamed | Refused | null> {
        return this.cannot("rename", at);
    }

    /// A request inspect cannot answer fails at once.
    private cannot(request: string, where: Place): never {
        const place = typeof where === "string" ? where : where.path;
        throw new Error(
            `clice inspect answers no ${request} (asked about ${place}): the test declares paths: { server: why }`,
        );
    }

    private text(file: string): string {
        return withoutBom(fs.readFileSync(path.join(this.root, file), "utf8"));
    }

    private range(file: string, raw: RawRange): Range {
        const source = this.context().file(file);
        return { start: source.positionAtByte(raw.begin), end: source.positionAtByte(raw.end) };
    }

    private location(file: string, raw: RawRange): Location {
        return { path: file, range: this.range(file, raw) };
    }

    /// A token split at its line ends, a line's "\r" left out and an empty
    /// piece dropped, as the server splits it.
    private pieces(source: SourceText, raw: RawToken): Token[] {
        const start = source.positionAtByte(raw.range.begin);
        const end = source.positionAtByte(raw.range.end);
        const type = lowerFirst(raw.kind);
        const modifiers = SYMBOL_MODIFIERS.filter((_name, bit) => raw.modifiers & (1 << bit)).map(
            lowerFirst,
        );
        const out: Token[] = [];
        for (let line = start.line; line <= end.line; line += 1) {
            const from = line === start.line ? start.character : 0;
            const to = line === end.line ? end.character : source.line(line).length;
            const length = Math.min(to, source.line(line).length) - from;
            if (length > 0) {
                out.push({ line, character: from, length, type, modifiers });
            }
        }
        return out;
    }

    /// A whole-document feature's answer for `file`.
    private async document(feature: string, file: string): Promise<unknown> {
        const key = `${feature}\0${file}`;
        let entry = this.documents.get(key);
        if (entry === undefined) {
            entry = this.run(feature, file).then((found) => this.record(file, found));
            this.documents.set(key, entry);
        }
        return (await entry).result;
    }

    /// A point feature's answer at `at`, from the run that asked at every
    /// marker of its file.
    private atPoint(feature: string, at: Anchor): Promise<unknown> {
        const text = this.text(at.path);
        const point = byteOf(text, locate(text, at));
        const batch = (): Places => ({
            points: Object.fromEntries(
                this.marks
                    .filter((mark) => mark.path === at.path)
                    .map((mark) => {
                        const byte = byteOf(text, mark.begin);
                        return [String(byte), byte];
                    }),
            ),
        });
        return this.batched(feature, at.path, String(point), batch, {
            points: { [String(point)]: point },
        });
    }

    /// A range feature's answer over `range` of `file`, from the run that
    /// asked over every marker of the file: a range's span, a point's
    /// cursor.
    private overRange(feature: string, file: string, range: Range): Promise<unknown> {
        const source = this.context().file(file);
        const span = {
            begin: byteOf(source.text, source.offsetAt(range.start)),
            end: byteOf(source.text, source.offsetAt(range.end)),
        };
        const batch = (): Places => ({
            ranges: Object.fromEntries(
                this.marks
                    .filter((mark) => mark.path === file)
                    .map((mark) => {
                        const marked = {
                            begin: byteOf(source.text, mark.begin),
                            end: byteOf(source.text, mark.end),
                        };
                        return [rangeKey(marked), marked];
                    }),
            ),
        });
        return this.batched(feature, file, rangeKey(span), batch, {
            ranges: { [rangeKey(span)]: span },
        });
    }

    private async batched(
        feature: string,
        file: string,
        key: string,
        batch: () => Places,
        alone: Places,
    ): Promise<unknown> {
        const id = `${feature}\0${file}`;
        let entry = this.batches.get(id);
        if (entry === undefined) {
            const places = batch();
            entry = this.run(feature, file, {
                points: { ...places.points, ...alone.points },
                ranges: { ...places.ranges, ...alone.ranges },
            }).then((found) => this.record(file, found));
            this.batches.set(id, entry);
        }
        const markers = (await entry).markers ?? {};
        if (key in markers) {
            return markers[key];
        }
        return (await this.run(feature, file, alone)).markers?.[key];
    }

    private record(file: string, entry: Entry): Entry {
        this.compiled.set(file, entry.diagnostics ?? []);
        return entry;
    }

    /// One `clice inspect` of `file`; a failed compile or run fails.
    private async run(feature: string, file: string, places?: Places): Promise<Entry> {
        if (this.importers.has(file)) {
            this.cannot(`${feature} in a module importer`, file);
        }
        const args = ["inspect", feature, path.join(this.root, file)];
        if (places !== undefined) {
            this.runs += 1;
            const table = path.join(this.scratch, `markers-${String(this.runs)}.json`);
            fs.writeFileSync(table, JSON.stringify(places));
            args.push(`--markers=${table}`);
        }
        if (this.config !== undefined) {
            args.push(`--config-overlay=${JSON.stringify(this.config)}`);
        }
        recordInvocation(args);
        const result = await runProcess(cliceExecutable(), args, {
            env: serverEnv(),
            timeout: 300_000,
        });
        if (result.status !== 0) {
            throw new Error(`clice inspect exited ${String(result.status)}:\n${result.stderr}`);
        }
        const entry = Object.values((JSON.parse(result.stdout) as Output).files)[0];
        if (entry === undefined) {
            throw new Error(`clice inspect answered nothing for ${file}`);
        }
        if (entry.error) {
            throw new Error(
                `${file}: clice inspect failed (${entry.error})\n  ${(entry.diagnostics ?? []).join("\n  ")}`,
            );
        }
        return entry;
    }
}
