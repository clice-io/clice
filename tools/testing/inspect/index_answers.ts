/// Where the index answers differently from the AST: the server answers a
/// file from its index until the file's AST is current, then from the AST,
/// and what the editor shows must not change in between. What the index's
/// entries give must be what the AST gives; what it leaves out passes — a
/// token, a modifier, a fold's kind, an outline entry — and so does what the
/// lexer alone gives where the index has no entry, which the AST refines
/// (a template's `<`, a format string's pieces, inactive code, a
/// `#pragma mark`).

import type { Fold, Link, OutlineSymbol, Token } from "../render/answers.ts";
import type { Range } from "../render/position.ts";

function at(range: Range): string {
    return `${String(range.start.line + 1)}:${String(range.start.character + 1)}`;
}

function sameRange(a: Range, b: Range): boolean {
    return (
        a.start.line === b.start.line &&
        a.start.character === b.start.character &&
        a.end.line === b.end.line &&
        a.end.character === b.end.character
    );
}

function spell(token: Token): string {
    return [token.type, ...[...token.modifiers].sort()].join(" ");
}

/// The token types the lexer gives a token no entry of the index names; a
/// user-defined operator or literal takes its modifier from an entry.
const LEXICAL = new Set([
    "comment",
    "string",
    "character",
    "number",
    "escape",
    "keyword",
    "primitive",
    "operator",
    "bracket",
    "directive",
    "header",
    "macroParameter",
    "attribute",
    "plain",
]);

function lexical(token: Token): boolean {
    return LEXICAL.has(token.type) && !token.modifiers.includes("userDefined");
}

function agrees(ast: Token, index: Token): boolean {
    return (
        ast.length === index.length &&
        ast.type === index.type &&
        index.modifiers.every((modifier) => ast.modifiers.includes(modifier))
    );
}

export function tokenDifferences(
    file: string,
    ast: readonly Token[],
    index: readonly Token[],
): string[] {
    const byStart = new Map<string, Token>();
    for (const token of ast) {
        byStart.set(`${String(token.line)}:${String(token.character)}`, token);
    }
    const differences: string[] = [];
    for (const token of index) {
        if (lexical(token)) {
            continue;
        }
        const where = `${file}:${String(token.line + 1)}:${String(token.character + 1)}`;
        const twin = byStart.get(`${String(token.line)}:${String(token.character)}`);
        if (twin === undefined) {
            differences.push(`semantic tokens ${where}: only the index has ${spell(token)}`);
            continue;
        }
        if (!agrees(twin, token)) {
            differences.push(
                `semantic tokens ${where}: the AST has ${spell(twin)} over ${String(twin.length)}, ` +
                    `the index ${spell(token)} over ${String(token.length)}`,
            );
        }
    }
    return differences;
}

export function foldDifferences(
    file: string,
    ast: readonly Fold[],
    index: readonly Fold[],
): string[] {
    const differences: string[] = [];
    for (const fold of index) {
        const twin = ast.find((other) => sameRange(other.range, fold.range));
        const where = `${file}:${at(fold.range)}`;
        if (twin === undefined) {
            differences.push(`folding ranges ${where}: only the index folds here`);
        } else if (
            (fold.kind !== undefined && twin.kind !== fold.kind) ||
            twin.collapsedText !== fold.collapsedText
        ) {
            differences.push(
                `folding ranges ${where}: the AST folds as ${twin.kind ?? "-"} ` +
                    `'${twin.collapsedText ?? ""}', the index as ${fold.kind ?? "-"} ` +
                    `'${fold.collapsedText ?? ""}'`,
            );
        }
    }
    return differences;
}

interface Flat {
    symbol: OutlineSymbol;
    parent: number;
}

/// The entries but the `#pragma mark`s (kind File), which the lexer alone
/// finds; what a mark sections nests in the mark's parent.
function flatten(symbols: readonly OutlineSymbol[], parent: number, out: Flat[]): Flat[] {
    for (const symbol of symbols) {
        if (symbol.kind === "File") {
            flatten(symbol.children, parent, out);
            continue;
        }
        const self = out.length;
        out.push({ symbol, parent });
        flatten(symbol.children, self, out);
    }
    return out;
}

export function outlineDifferences(
    file: string,
    ast: readonly OutlineSymbol[],
    index: readonly OutlineSymbol[],
): string[] {
    const left = flatten(ast, -1, []);
    const right = flatten(index, -1, []);
    // An index entry pairs with the AST's entry of the same name range, the
    // same name first.
    const taken = new Set<number>();
    const partner = right.map(({ symbol }) => {
        const candidates = left
            .map((flat, position) => ({ flat, position }))
            .filter(
                ({ flat, position }) =>
                    !taken.has(position) &&
                    sameRange(flat.symbol.selection.range, symbol.selection.range),
            );
        const chosen =
            candidates.find(({ flat }) => flat.symbol.name === symbol.name) ?? candidates[0];
        if (chosen === undefined) {
            return -1;
        }
        taken.add(chosen.position);
        return chosen.position;
    });
    const name = (flats: readonly Flat[], position: number): string =>
        position === -1 ? "(top level)" : (flats[position]?.symbol.name ?? "?");
    const differences: string[] = [];
    right.forEach(({ symbol, parent }, position) => {
        const where = `document symbols ${file}:${at(symbol.selection.range)}`;
        const twin = partner[position] ?? -1;
        const other = left[twin];
        if (other === undefined) {
            differences.push(`${where}: only the index lists ${symbol.kind} ${symbol.name}`);
            return;
        }
        const theirs = other.symbol;
        const report = (field: string, ours: string, astValue: string): void => {
            differences.push(`${where}: ${field} is ${astValue} in the AST, ${ours} in the index`);
        };
        if (theirs.name !== symbol.name) {
            report("the name", symbol.name, theirs.name);
        }
        if (theirs.kind !== symbol.kind) {
            report("the kind", symbol.kind, theirs.kind);
        }
        if (
            symbol.range !== undefined &&
            theirs.range !== undefined &&
            !sameRange(theirs.range.range, symbol.range.range)
        ) {
            report("the range", at(symbol.range.range), at(theirs.range.range));
        }
        // A parent only the index lists is no top level of the AST's.
        const parentTwin = parent === -1 ? -1 : (partner[parent] ?? -1);
        if ((parent !== -1 && parentTwin === -1) || parentTwin !== other.parent) {
            report("the parent", name(right, parent), name(left, other.parent));
        }
        if (
            symbol.detail !== undefined &&
            symbol.detail !== "" &&
            symbol.detail !== theirs.detail
        ) {
            report("the detail", symbol.detail, theirs.detail ?? "");
        }
        if ((symbol.tags ?? []).join(",") !== (theirs.tags ?? []).join(",")) {
            report("the tags", (symbol.tags ?? []).join(","), (theirs.tags ?? []).join(","));
        }
    });
    return differences;
}

export function linkDifferences(
    file: string,
    ast: readonly Link[],
    index: readonly Link[],
): string[] {
    const differences: string[] = [];
    for (const link of index) {
        const twin = ast.find((other) => sameRange(other.location.range, link.location.range));
        const where = `document links ${file}:${at(link.location.range)}`;
        if (twin === undefined) {
            differences.push(`${where}: only the index links to ${link.target}`);
        } else if (twin.target !== link.target) {
            differences.push(
                `${where}: the AST links to ${twin.target}, the index to ${link.target}`,
            );
        }
    }
    return differences;
}
