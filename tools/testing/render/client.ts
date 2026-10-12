/// The requests every client answers, `clice inspect` and a server alike,
/// each rendered one way: a client decodes its replies into the shapes of
/// answers.ts, this class renders them.

import { locate, spanOf, type Anchor, type MarkerAnchor } from "../cases/anchors.ts";
import {
    renderActions,
    renderCalls,
    renderCompletion,
    renderCounterparts,
    renderFolds,
    renderFormatting,
    renderHighlights,
    renderHover,
    renderInlayHints,
    renderHierarchyItems,
    renderLinks,
    renderLocations,
    renderOutline,
    renderPrepareRename,
    renderRename,
    renderSelection,
    renderSignatures,
    renderTokenAt,
    renderTokens,
    renderWorkspaceSymbols,
    type Action,
    type Call,
    type Completion,
    type Counterparts,
    type Fold,
    type Highlight,
    type Hover,
    type InlayHint,
    type Item,
    type Link,
    type OutlineSymbol,
    type Refused,
    type Renameable,
    type Renamed,
    type Signatures,
    type Token,
    type WorkspaceSymbol,
} from "./answers.ts";
import type { TextEdit } from "./edits.ts";
import type { AnswerContext } from "./lsp.ts";
import type { Location, Named, Range, RenderScope, SourceText } from "./position.ts";
import { shown } from "./shown.ts";

/// A file, or a place in one.
export type Place = string | Anchor;

/// The span of text a request about `at` covers: a range marker's range,
/// the cursor of anything else.
function selectionOf(text: string, at: Anchor): { begin: number; end: number } {
    if ("name" in at && at.range) {
        return spanOf(text, at);
    }
    const cursor = locate(text, at);
    return { begin: cursor, end: cursor };
}

export abstract class Client {
    /// The case's markers: the named ranges among them render a location
    /// equal to one as its name, while their file reads as the case wrote it.
    protected readonly marks: readonly MarkerAnchor[];
    /// Compile checks edit the files they check, so they run one at a time.
    private checks: Promise<unknown> = Promise.resolve();

    constructor(marks: readonly MarkerAnchor[]) {
        this.marks = marks;
    }

    /// Reads files as the requests see them and renders paths.
    protected abstract context(): AnswerContext;

    // What each client decodes.

    protected abstract fetchHover(at: Anchor): Promise<Hover | null>;
    protected abstract fetchCompletion(
        at: Anchor,
        trigger: string | undefined,
    ): Promise<Completion>;
    protected abstract fetchSignatureHelp(at: Anchor): Promise<Signatures>;
    protected abstract fetchHighlights(at: Anchor): Promise<Highlight[]>;
    protected abstract fetchSelectionRange(at: Anchor): Promise<Range[]>;
    /// The code actions over `selection` of `file`.
    protected abstract fetchCodeActions(
        file: string,
        selection: Range,
        only: readonly string[] | undefined,
    ): Promise<Action[]>;
    /// The errors `file` compiles with once `edits` apply; the files are
    /// restored after.
    protected abstract compileWith(
        file: string,
        edits: ReadonlyMap<string, readonly TextEdit[]>,
    ): Promise<string[]>;
    protected abstract fetchLocations(
        request: LocationRequest,
        at: Anchor,
        options?: ReferenceOptions,
    ): Promise<Location[]>;
    /// The items a call or type hierarchy opens on at `at`.
    protected abstract fetchHierarchy(hierarchy: "call" | "type", at: Anchor): Promise<Item[]>;
    protected abstract fetchCalls(direction: "incoming" | "outgoing", at: Anchor): Promise<Call[]>;
    protected abstract fetchTypes(
        direction: "supertypes" | "subtypes",
        at: Anchor,
    ): Promise<Item[]>;
    protected abstract fetchTokens(file: string): Promise<Token[]>;
    protected abstract fetchInlayHints(file: string, range: Range): Promise<InlayHint[]>;
    protected abstract fetchFolds(file: string): Promise<Fold[]>;
    protected abstract fetchSymbols(file: string): Promise<OutlineSymbol[]>;
    protected abstract fetchLinks(file: string): Promise<Link[]>;
    protected abstract fetchCounterparts(file: string): Promise<Counterparts>;
    protected abstract fetchWorkspaceSymbols(query: string): Promise<WorkspaceSymbol[]>;
    /// The edits formatting `file`, or `range` of it, makes.
    protected abstract fetchFormatting(
        file: string,
        range: Range | undefined,
    ): Promise<TextEdit[] | null>;
    protected abstract fetchPrepareRename(at: Anchor): Promise<Renameable | Refused | null>;
    protected abstract fetchRename(at: Anchor, newName: string): Promise<Renamed | Refused | null>;

    // The requests.

    hover(at: Anchor): Promise<Hover | null> {
        const scope = this.scopeAt(at);
        return shown(this.fetchHover(at), "hover", (hover) => renderHover(hover, scope));
    }

    /// The completion at `at`, as typing `trigger` asks for it.
    completion(at: Anchor, trigger?: string): Promise<Completion> {
        const scope = this.scopeAt(at);
        return shown(
            this.fetchCompletion(at, trigger),
            "completion",
            (completion, { parts, full }) =>
                renderCompletion(completion, {
                    scope,
                    parts,
                    ...(full ? { limit: completion.items.length } : {}),
                }),
        );
    }

    signatureHelp(at: Anchor): Promise<Signatures> {
        return shown(this.fetchSignatureHelp(at), "signature_help", renderSignatures);
    }

    highlights(at: Anchor): Promise<Highlight[]> {
        const scope = this.scopeAt(at);
        return shown(this.fetchHighlights(at), "highlights", (highlights) =>
            renderHighlights(highlights, scope),
        );
    }

    /// The ranges around `at`, innermost first.
    selectionRange(at: Anchor): Promise<Range[]> {
        const scope = this.scopeAt(at);
        return shown(this.fetchSelectionRange(at), "selection_ranges", (ranges) =>
            renderSelection(at.path, ranges, scope),
        );
    }

    /// The code actions over `at`, a range marker's range or the cursor;
    /// `only` filters them by kind as an editor asks, and `compile` checks
    /// what each one's edits compile to.
    codeActions(at: Anchor, options: { only?: string[]; compile?: true } = {}): Promise<Action[]> {
        const context = this.context();
        const source = context.file(at.path);
        const { begin, end } = selectionOf(source.text, at);
        const selection = { start: source.positionAt(begin), end: source.positionAt(end) };
        // What the edits apply to, read before a check rewrites the files:
        // the diffs render from it.
        const before = new Map<string, SourceText>();
        const reply = this.fetchCodeActions(at.path, selection, options.only).then(
            async (actions) => {
                if (options.compile !== true) {
                    return actions;
                }
                const check = this.checks.then(async () => {
                    for (const file of actions.flatMap((action) => [...action.edits.keys()])) {
                        before.set(file, context.file(file));
                    }
                    const out: Action[] = [];
                    for (const action of actions) {
                        out.push(
                            action.edits.size === 0
                                ? action
                                : {
                                      ...action,
                                      errors: await this.compileWith(at.path, action.edits),
                                  },
                        );
                    }
                    return out;
                });
                this.checks = check.catch(() => undefined);
                return check;
            },
        );
        return shown(reply, "code_actions", (actions) =>
            renderActions(actions, (file) => before.get(file) ?? context.file(file)),
        );
    }

    definition(at: Anchor): Promise<Location[]> {
        return this.locations("definition", at);
    }

    declaration(at: Anchor): Promise<Location[]> {
        return this.locations("declaration", at);
    }

    typeDefinition(at: Anchor): Promise<Location[]> {
        return this.locations("type definition", at);
    }

    implementation(at: Anchor): Promise<Location[]> {
        return this.locations("implementation", at);
    }

    /// The references at `at`, the declarations and the definition among
    /// them unless `declarations: false` asks as an editor's
    /// includeDeclaration off does.
    references(at: Anchor, options: ReferenceOptions = {}): Promise<Location[]> {
        return this.locations("references", at, options);
    }

    /// The items a call hierarchy opens on at `at`, before any call.
    callHierarchy(at: Anchor): Promise<Item[]> {
        return this.hierarchy("call", at);
    }

    typeHierarchy(at: Anchor): Promise<Item[]> {
        return this.hierarchy("type", at);
    }

    /// The calls into each call hierarchy item at `at`.
    incomingCalls(at: Anchor): Promise<Call[]> {
        return this.calls("incoming", at);
    }

    outgoingCalls(at: Anchor): Promise<Call[]> {
        return this.calls("outgoing", at);
    }

    /// The direct supertypes of each type hierarchy item at `at`.
    supertypes(at: Anchor): Promise<Item[]> {
        return this.types("supertypes", at);
    }

    subtypes(at: Anchor): Promise<Item[]> {
        return this.types("subtypes", at);
    }

    /// Every semantic token of a file, or the one a place lies in, as an
    /// editor's token inspector shows it.
    semanticTokens(file: string): Promise<Token[]>;
    semanticTokens(at: Anchor): Promise<Token | null>;
    semanticTokens(place: Place): Promise<Token[] | Token | null> {
        if (typeof place === "string") {
            const scope = this.scopeOf(place);
            return shown(this.fetchTokens(place), "semantic_tokens", (tokens) =>
                renderTokens(place, tokens, scope),
            );
        }
        const source = this.context().file(place.path);
        const { line, character } = source.positionAt(locate(source.text, place));
        const reply = this.fetchTokens(place.path).then(
            (tokens) =>
                tokens.find(
                    (token) =>
                        token.line === line &&
                        token.character <= character &&
                        character < token.character + token.length,
                ) ?? null,
        );
        return shown(reply, "semantic_token", (token) =>
            renderTokenAt(source.line(line), character, token),
        );
    }

    /// The inlay hints of a whole file, or over a place: a range marker's
    /// range, a snippet's text.
    inlayHints(place: Place): Promise<InlayHint[]> {
        const file = typeof place === "string" ? place : place.path;
        const source = this.context().file(file);
        let range: Range;
        if (typeof place === "string") {
            range = {
                start: { line: 0, character: 0 },
                end: source.positionAt(source.text.length),
            };
        } else {
            const { begin, end } = spanOf(source.text, place);
            range = { start: source.positionAt(begin), end: source.positionAt(end) };
        }
        const scope = this.scopeOf(file);
        return shown(this.fetchInlayHints(file, range), "inlay_hints", (hints, { parts }) =>
            renderInlayHints(file, hints, scope, parts),
        );
    }

    foldingRanges(file: string): Promise<Fold[]> {
        const scope = this.scopeOf(file);
        return shown(this.fetchFolds(file), "folding_ranges", (folds) =>
            renderFolds(file, folds, scope),
        );
    }

    documentSymbols(file: string): Promise<OutlineSymbol[]> {
        const scope = this.scopeOf(file);
        return shown(this.fetchSymbols(file), "document_symbols", (symbols, { parts }) =>
            renderOutline(symbols, scope, parts),
        );
    }

    documentLinks(file: string): Promise<Link[]> {
        const scope = this.scopeOf(file);
        return shown(this.fetchLinks(file), "document_links", (links) => renderLinks(links, scope));
    }

    /// The files `file` switches to (clice/counterparts), best first.
    counterparts(file: string): Promise<Counterparts> {
        return shown(this.fetchCounterparts(file), "counterparts", renderCounterparts);
    }

    workspaceSymbols(query: string): Promise<WorkspaceSymbol[]> {
        const scope = this.context().scope(undefined, this.names());
        return shown(this.fetchWorkspaceSymbols(query), "workspace_symbols", (symbols) =>
            renderWorkspaceSymbols(symbols, scope),
        );
    }

    /// What formatting a whole file, or the range of a place (a range
    /// marker's range, a snippet's text), does to the file.
    formatting(place: Place): Promise<TextEdit[] | null> {
        const file = typeof place === "string" ? place : place.path;
        const source = this.context().file(file);
        let range: Range | undefined;
        if (typeof place !== "string") {
            const { begin, end } = spanOf(source.text, place);
            range = { start: source.positionAt(begin), end: source.positionAt(end) };
        }
        return shown(this.fetchFormatting(file, range), "formatting", (edits) =>
            renderFormatting(source, edits),
        );
    }

    /// Whether the name at `at` can be renamed: its range and placeholder,
    /// or why not.
    prepareRename(at: Anchor): Promise<Renameable | Refused | null> {
        const scope = this.scopeAt(at);
        return shown(this.fetchPrepareRename(at), "prepare_rename", (answer) =>
            renderPrepareRename(answer, scope),
        );
    }

    /// The edits renaming the symbol at `at` to `newName` makes, or why not;
    /// `compile` checks what the file at `at` compiles to with them.
    rename(
        at: Anchor,
        newName: string,
        options: { compile?: true } = {},
    ): Promise<Renamed | Refused | null> {
        const context = this.context();
        // What the edits apply to, read as the reply arrives, before a check
        // rewrites the files.
        const before = new Map<string, SourceText>();
        const reply = this.fetchRename(at, newName).then((answer) => {
            if (answer === null || "error" in answer) {
                return answer;
            }
            for (const file of answer.edits.keys()) {
                before.set(file, context.file(file));
            }
            if (options.compile !== true) {
                return answer;
            }
            const check = this.checks.then(async () => ({
                ...answer,
                errors: await this.compileWith(at.path, answer.edits),
            }));
            this.checks = check.catch(() => undefined);
            return check;
        });
        return shown(reply, "rename", (answer, { parts }) =>
            renderRename(answer, (file) => before.get(file) ?? context.file(file), parts),
        );
    }

    // Rendering.

    /// The named ranges whose file reads as the case wrote it.
    protected names(): Named[] {
        const context = this.context();
        return this.marks.flatMap((mark) => {
            const source = context.peek(mark.path);
            if (!mark.range || mark.name.startsWith("nameless_") || source?.text !== mark.text) {
                return [];
            }
            const range = {
                start: source.positionAt(mark.begin),
                end: source.positionAt(mark.end),
            };
            return [{ name: mark.name, location: { path: mark.path, range } }];
        });
    }

    /// The scope of an answer about a whole file.
    protected scopeOf(file: string): RenderScope {
        return this.context().scope({ path: file }, this.names());
    }

    /// The scope of an answer about a place: same-file offsets count from
    /// its line.
    protected scopeAt(at: Anchor): RenderScope {
        const context = this.context();
        const source = context.file(at.path);
        const line = source.positionAt(locate(source.text, at)).line;
        return context.scope({ path: at.path, line }, this.names());
    }

    private locations(
        request: LocationRequest,
        at: Anchor,
        options: ReferenceOptions = {},
    ): Promise<Location[]> {
        const scope = this.scopeAt(at);
        return shown(
            this.fetchLocations(request, at, options),
            request.replace(" ", "_"),
            (locations) => renderLocations(locations, scope),
        );
    }

    private calls(direction: "incoming" | "outgoing", at: Anchor): Promise<Call[]> {
        const scope = this.scopeAt(at);
        return shown(this.fetchCalls(direction, at), `${direction}_calls`, (calls) =>
            renderCalls(calls, scope),
        );
    }

    private hierarchy(hierarchy: "call" | "type", at: Anchor): Promise<Item[]> {
        const scope = this.scopeAt(at);
        return shown(this.fetchHierarchy(hierarchy, at), `${hierarchy}_hierarchy`, (items) =>
            renderHierarchyItems(items, scope),
        );
    }

    private types(direction: "supertypes" | "subtypes", at: Anchor): Promise<Item[]> {
        const scope = this.scopeAt(at);
        return shown(this.fetchTypes(direction, at), direction, (items) =>
            renderHierarchyItems(items, scope),
        );
    }
}

export type LocationRequest =
    "definition" | "declaration" | "type definition" | "implementation" | "references";

/// How a references request asks: `declarations: false` leaves out the
/// declarations and the definition (LSP includeDeclaration).
export interface ReferenceOptions {
    declarations?: false;
}
