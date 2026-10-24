/// Positions rendered as the text they point at, and resolved back.
///
/// A range renders as the line it is on with the range set off in ⟦ ⟧,
/// leading indentation dropped: `double ⟦area⟧(const Circle& c);`. Another
/// file than the query's comes first (`include/shapes/circle.h │ ...`), a
/// range spanning lines shows its first and last line and the count, and a
/// line whose text is not unique in its file gets an offset (`@-9` from the
/// query in the same file, `@12` 1-based elsewhere). A range a fixture named
/// renders as its name.
///
/// `renderLocation` checks every rendering against `resolveLocation` and adds the
/// offset when the text alone does not single the range out, so a rendering
/// always resolves to exactly the range it came from: the grammar is
/// whatever `resolveLocation` accepts.

export interface Position {
    line: number;
    /// UTF-16 code units from the start of the line, as in LSP — a
    /// JavaScript string index into the line.
    character: number;
}

export interface Range {
    start: Position;
    end: Position;
}

export interface Location {
    /// Workspace-relative POSIX path, or a `${...}`-rooted one outside it.
    path: string;
    range: Range;
}

/// A file's text as an editor shows and sends it, and clice addresses it:
/// without a leading byte order mark.
export function withoutBom(text: string): string {
    return text.startsWith("\uFEFF") ? text.slice(1) : text;
}

/// The line whose start, of the ascending `starts`, is the last at or
/// before `offset`.
function lineAt(starts: readonly number[], offset: number): number {
    let lo = 0;
    let hi = starts.length - 1;
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if ((starts[mid] ?? 0) <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/// A file as answers address it: lines split at "\n", a trailing "\r"
/// belonging to the line ending.
export class SourceText {
    readonly path: string;
    readonly text: string;
    readonly lines: readonly string[];
    private readonly raw: readonly string[];
    private readonly starts: readonly number[];
    private readonly byteStarts: readonly number[];

    constructor(path: string, text: string) {
        this.path = path;
        this.text = text;
        const raw = text.split("\n");
        this.raw = raw;
        this.lines = raw.map((line) => (line.endsWith("\r") ? line.slice(0, -1) : line));
        const starts: number[] = [];
        const byteStarts: number[] = [];
        let offset = 0;
        let byteOffset = 0;
        for (const line of raw) {
            starts.push(offset);
            byteStarts.push(byteOffset);
            offset += line.length + 1;
            byteOffset += Buffer.byteLength(line, "utf8") + 1;
        }
        this.starts = starts;
        this.byteStarts = byteStarts;
    }

    line(index: number): string {
        const line = this.lines[index];
        if (line === undefined) {
            throw new Error(`${this.path}: no line ${index}`);
        }
        return line;
    }

    /// The position of a UTF-16 offset into the text.
    positionAt(offset: number): Position {
        const line = lineAt(this.starts, offset);
        return { line, character: offset - (this.starts[line] ?? 0) };
    }

    /// The position of a UTF-8 byte offset, which is how `clice inspect`
    /// addresses the file.
    positionAtByte(byteOffset: number): Position {
        const line = lineAt(this.byteStarts, byteOffset);
        const prefix = Buffer.from(this.raw[line] ?? "", "utf8")
            .subarray(0, byteOffset - (this.byteStarts[line] ?? 0))
            .toString("utf8");
        return { line, character: prefix.length };
    }

    offsetAt(position: Position): number {
        const start = this.starts[position.line];
        if (start === undefined) {
            throw new Error(`${this.path}: no line ${position.line}`);
        }
        return start + position.character;
    }
}

/// A range a fixture named, rendered as its name.
export interface Named {
    name: string;
    location: Location;
}

export interface RenderScope {
    file(path: string): SourceText;
    /// Where the request was made: the file a rendering omits, the line a
    /// same-file offset counts from. Without a line (a request about the
    /// whole document) a repeated line in the file gets its number instead.
    query?: { path: string; line?: number | undefined } | undefined;
    names?: readonly Named[] | undefined;
}

const OPEN = "⟦";
const CLOSE = "⟧";
const FILE_SEPARATOR = " │ ";
const SPAN = " … ";
const NAME = /^\w+$/;
const SUFFIX = / @([+-]?\d+)$/;
const SPANNED = / \((\d+) lines\)$/;

function sameRange(a: Range, b: Range): boolean {
    return (
        a.start.line === b.start.line &&
        a.start.character === b.start.character &&
        a.end.line === b.end.line &&
        a.end.character === b.end.character
    );
}

function indentOf(line: string): number {
    return line.length - line.trimStart().length;
}

function blank(text: string): boolean {
    return text.trim() === "";
}

/// The line text a rendering shows: indentation dropped unless the range
/// starts inside it, trailing whitespace dropped unless the range ends in
/// it. `from` is the index in `line` the shown text begins at.
function shown(line: string, start: number, end: number): { text: string; from: number } {
    const from = start >= indentOf(line) ? indentOf(line) : 0;
    const trimmed = line.trimEnd().length;
    const to = end <= trimmed ? trimmed : line.length;
    return { text: line.slice(from, to), from };
}

function body(source: SourceText, range: Range): string {
    const { start, end } = range;
    const first = source.line(start.line);
    if (start.line === end.line) {
        const view = shown(first, start.character, end.character);
        const a = start.character - view.from;
        const b = end.character - view.from;
        return `${view.text.slice(0, a)}${OPEN}${view.text.slice(a, b)}${CLOSE}${view.text.slice(b)}`;
    }
    const head = shown(first, start.character, first.length);
    const a = start.character - head.from;
    const last = source.line(end.line);
    const tail = last.slice(0, end.character);
    const lastShown = end.character > indentOf(last) ? tail.trimStart() : tail;
    const count = end.line - start.line + 1;
    return `${head.text.slice(0, a)}${OPEN}${head.text.slice(a).trimEnd()}${SPAN}${lastShown}${CLOSE} (${count} lines)`;
}

function nameOf(location: Location, scope: RenderScope): string | undefined {
    return scope.names?.find(
        (named) =>
            named.location.path === location.path &&
            sameRange(named.location.range, location.range),
    )?.name;
}

export function renderLocation(location: Location, scope: RenderScope): string {
    const name = nameOf(location, scope);
    if (name !== undefined) {
        return name;
    }
    const source = scope.file(location.path);
    const sameFile = scope.query?.path === location.path;
    const prefix = sameFile ? "" : `${location.path}${FILE_SEPARATOR}`;
    const text = `${prefix}${body(source, location.range)}`;
    const candidates = [text];
    if (sameFile && scope.query?.line !== undefined) {
        const delta = location.range.start.line - scope.query.line;
        candidates.push(`${text} @${delta >= 0 ? "+" : ""}${delta}`);
    }
    candidates.push(`${text} @${location.range.start.line + 1}`);
    for (const candidate of candidates) {
        const resolved = tryResolve(candidate, scope);
        if (resolved?.path === location.path && sameRange(resolved.range, location.range)) {
            return candidate;
        }
    }
    throw new Error(
        `cannot render ${location.path} ${JSON.stringify(location.range)} unambiguously`,
    );
}

export function resolveLocation(rendered: string, scope: RenderScope): Location {
    const location = tryResolve(rendered, scope);
    if (location === undefined) {
        throw new Error(`'${rendered}' names no unique range`);
    }
    return location;
}

function tryResolve(rendered: string, scope: RenderScope): Location | undefined {
    if (NAME.test(rendered)) {
        return scope.names?.find((named) => named.name === rendered)?.location;
    }
    let rest = rendered;
    let path = scope.query?.path;
    const separator = rest.indexOf(FILE_SEPARATOR);
    if (separator >= 0) {
        path = rest.slice(0, separator);
        rest = rest.slice(separator + FILE_SEPARATOR.length);
    }
    if (path === undefined) {
        return undefined;
    }
    let line: number | undefined;
    const suffix = SUFFIX.exec(rest);
    if (suffix) {
        const value = suffix[1] ?? "";
        rest = rest.slice(0, suffix.index);
        if (value.startsWith("+") || value.startsWith("-")) {
            if (scope.query?.path !== path || scope.query.line === undefined) {
                return undefined;
            }
            line = scope.query.line + Number(value);
        } else {
            line = Number(value) - 1;
        }
    }
    const source = scope.file(path);
    const spanned = SPANNED.exec(rest);
    const found = spanned
        ? resolveSpanned(source, rest.slice(0, spanned.index), Number(spanned[1]), line)
        : resolveLine(source, rest, line);
    return found && { path, range: found };
}

function indicesOf(text: string, mark: string): number[] {
    const out: number[] = [];
    for (let at = text.indexOf(mark); at >= 0; at = text.indexOf(mark, at + 1)) {
        out.push(at);
    }
    return out;
}

/// Every way a rendered line sets off a range: a source line may hold the
/// marks itself, so each ⟦ before a ⟧ is a candidate, and the rendering of
/// the range a candidate names decides.
function markers(text: string): { plain: string; open: number; close: number }[] {
    const closes = indicesOf(text, CLOSE);
    return indicesOf(text, OPEN).flatMap((open) =>
        closes
            .filter((close) => close > open)
            .map((close) => ({
                plain: text.slice(0, open) + text.slice(open + 1, close) + text.slice(close + 1),
                open,
                close: close - 1,
            })),
    );
}

/// Every way a line can be shown, with the index its shown text begins at.
function views(line: string): { text: string; from: number }[] {
    const indent = indentOf(line);
    const trimmed = line.trimEnd().length;
    return [
        { text: line.slice(indent, trimmed), from: indent },
        { text: line.slice(0, trimmed), from: 0 },
        { text: line.slice(indent), from: indent },
        { text: line, from: 0 },
    ];
}

function unique<T>(
    matches: T[],
    pick: (match: T) => number,
    line: number | undefined,
): T | undefined {
    const narrowed = line === undefined ? matches : matches.filter((match) => pick(match) === line);
    const first = narrowed[0];
    return narrowed.length === 1 ? first : undefined;
}

function resolveLine(
    source: SourceText,
    text: string,
    line: number | undefined,
): Range | undefined {
    const matches: Range[] = [];
    for (const marked of markers(text)) {
        source.lines.forEach((candidate, index) => {
            for (const view of views(candidate)) {
                if (view.text === marked.plain) {
                    const range = {
                        start: { line: index, character: view.from + marked.open },
                        end: { line: index, character: view.from + marked.close },
                    };
                    if (body(source, range) === text) {
                        matches.push(range);
                        return;
                    }
                }
            }
        });
    }
    return unique(matches, (range) => range.start.line, line);
}

function resolveSpanned(
    source: SourceText,
    text: string,
    count: number,
    line: number | undefined,
): Range | undefined {
    const split = text.indexOf(SPAN);
    const close = text.lastIndexOf(CLOSE);
    if (split < 0 || close < split) {
        return undefined;
    }
    const headText = text.slice(0, split);
    const lastShown = text.slice(split + SPAN.length, close);
    const matches: Range[] = [];
    for (const open of indicesOf(headText, OPEN)) {
        const firstPlain = headText.slice(0, open) + headText.slice(open + 1);
        source.lines.forEach((candidate, index) => {
            const endLine = index + count - 1;
            const last = source.lines[endLine];
            if (last === undefined) {
                return;
            }
            const endCharacter = blank(lastShown)
                ? lastShown.length
                : indentOf(last) + lastShown.length;
            for (const view of views(candidate)) {
                if (
                    view.text.trimEnd() === firstPlain.trimEnd() &&
                    view.text.startsWith(firstPlain)
                ) {
                    const range = {
                        start: { line: index, character: view.from + open },
                        end: { line: endLine, character: endCharacter },
                    };
                    if (
                        endCharacter <= last.length &&
                        body(source, range) === `${text} (${count} lines)`
                    ) {
                        matches.push(range);
                        return;
                    }
                }
            }
        });
    }
    return unique(matches, (range) => range.start.line, line);
}
