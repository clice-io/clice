/// Markdown segmentation shared by the translation tools. A page is split
/// into top-level segments; translatable ones (headings, paragraphs,
/// blockquotes, list items, table rows, YAML frontmatter) carry prose,
/// everything else (code blocks, HTML comments, ...) is verbatim. The
/// en↔zh contract in translate.ts compares these sequences and its
/// translate mode feeds the translatable ones to a model.

import { createHash } from "node:crypto";
import type { Nodes, RootContent, Table } from "mdast";
import remarkFrontmatter from "remark-frontmatter";
import remarkGfm from "remark-gfm";
import remarkParse from "remark-parse";
import { unified } from "unified";
import type { Point } from "unist";
import { parse as parseYaml } from "yaml";

export interface Range {
    start: number;
    end: number;
}

export interface Segment {
    start: number;
    end: number;
    kind: string;
    /// `kind` plus the structure the contract compares beyond the node
    /// type: heading depth, list orderedness, table cell count.
    shape: string;
    translatable: boolean;
    /// Byte ranges that must be identical across the two trees: the whole
    /// segment when it is not translatable, otherwise the fenced code
    /// blocks nested inside it (a snap example under a capability's text).
    verbatim: Range[];
    /// The name a heading or a table body row carries — the heading text,
    /// the first cell — as markdown source; null elsewhere. A row and a
    /// later heading with the same label in en name one thing (a
    /// capability's status row and its section), so zh must keep them
    /// equal too.
    label: string | null;
    /// Inline literals a translation carries over unchanged, as a sorted
    /// set. Inline code and issue references stand alone: prose may
    /// reorder or repeat them, not alter them. Link and image
    /// destinations are keyed by their position among the segment's links
    /// (`link[0]: ./x`), so each label keeps its own target — a
    /// translation keeps the links in order — and frontmatter control
    /// values (layout, theme, icon, link, ...) by the YAML key holding
    /// them, as the block's structure is fixed.
    literals: string[];
}

/// A segment enriched with everything comparisons need. `index` is the
/// 1-based position in the full segment sequence (translatable and
/// verbatim alike), so messages number segments consistently.
export interface SegmentInfo {
    index: number;
    kind: string;
    shape: string;
    translatable: boolean;
    text: string;
    hash: string;
    line: number;
    verbatim: string[];
    label: string | null;
    literals: string[];
}

export const parser = unified().use(remarkParse).use(remarkGfm).use(remarkFrontmatter);

function offsetOf(point: Point | undefined, page: string): number {
    if (point?.offset === undefined) {
        throw new Error(`${page}: parser produced a node without source offsets`);
    }
    return point.offset;
}

function rangeOf(node: Nodes, page: string): Range {
    return {
        start: offsetOf(node.position?.start, page),
        end: offsetOf(node.position?.end, page),
    };
}

/// Column alignment of a table as one letter per column: `l`, `r`, `c`,
/// or `-` for none. Only the delimiter line carries it, and that line has
/// no row node, so rows get it from their table.
export function tableAlign(table: Table): string {
    return (table.align ?? []).map((align) => align?.[0] ?? "-").join("");
}

/// Mapping and sequence nesting of a YAML block, with key names and
/// scalar types; a translation may only change scalar string values.
function yamlSkeleton(value: unknown): string {
    if (Array.isArray(value)) {
        return `[${value.map(yamlSkeleton).join(",")}]`;
    }
    if (value !== null && typeof value === "object") {
        const entries = Object.entries(value as Record<string, unknown>);
        return `{${entries.map(([key, item]) => `${key}:${yamlSkeleton(item)}`).join(",")}}`;
    }
    return value === null ? "null" : typeof value;
}

/// Block structure, recursively through flow containers (blockquotes,
/// lists, tables); everything below a paragraph, heading or table cell is
/// phrasing, which a translation may reflow freely.
function shapeOf(node: Nodes, ordered: boolean, align = ""): string {
    switch (node.type) {
        case "heading":
            return `heading:${node.depth}`;
        case "tableRow":
            return `tableRow:${node.children.length}:${align}`;
        case "table": {
            const inner = node.children.map((row) => shapeOf(row, false, tableAlign(node)));
            return `table(${inner.join(",")})`;
        }
        case "list": {
            const inner = node.children.map((child) => shapeOf(child, node.ordered === true));
            return `${node.ordered === true ? "list:ordered" : "list"}(${inner.join(",")})`;
        }
        case "listItem": {
            const inner = node.children.map((child) => shapeOf(child, false));
            const checked =
                node.checked === true ? ":checked" : node.checked === false ? ":unchecked" : "";
            return `listItem${ordered ? ":ordered" : ""}${checked}(${inner.join(",")})`;
        }
        case "blockquote":
            return `blockquote(${node.children.map((child) => shapeOf(child, false)).join(",")})`;
        case "yaml": {
            try {
                return `yaml${yamlSkeleton(parseYaml(node.value))}`;
            } catch {
                return "yaml:invalid";
            }
        }
        default:
            return node.type;
    }
}

/// Code blocks and HTML comments anywhere below `node` (inline code may
/// move with the prose; `literals` holds it to its value). A code range
/// starts at the beginning of the opening fence's line, so the list or
/// blockquote prefix in front of the fence is compared too — it decides
/// how much indentation is stripped from the block's lines.
function nestedVerbatim(node: Nodes, source: string, from: number, page: string): Range[] {
    const out: Range[] = [];
    const visit = (current: Nodes) => {
        if (current.type === "code") {
            const range = rangeOf(current, page);
            const lineStart = source.lastIndexOf("\n", range.start - 1) + 1;
            out.push({ start: Math.max(lineStart, from), end: range.end });
        } else if (current.type === "html" && current.value.startsWith("<!--")) {
            out.push(rangeOf(current, page));
        } else if ("children" in current) {
            for (const child of current.children) {
                visit(child);
            }
        }
    };
    if ("children" in node) {
        for (const child of node.children) {
            visit(child);
        }
    }
    return out;
}

/// Source text of a node's phrasing content — a heading without its
/// markers, a table cell without its padding — or null when it is empty.
function phrasingOf(node: Nodes, source: string, page: string): string | null {
    if (!("children" in node)) {
        return null;
    }
    const first = node.children.at(0);
    const last = node.children.at(-1);
    if (first === undefined || last === undefined) {
        return null;
    }
    return source.slice(rangeOf(first, page).start, rangeOf(last, page).end);
}

/// Frontmatter keys whose string values are reader-facing copy — the
/// VitePress hero and feature text, page titles and descriptions — and so
/// translate. A string under any other key (layout, theme, icon, link,
/// src, ...) is a control value the site reads; a key missing here fails
/// closed, the check reports the translated value under its key.
export const YAML_PROSE_KEYS = new Set([
    "name",
    "text",
    "tagline",
    "title",
    "details",
    "alt",
    "linkText",
    "description",
]);

/// Scalars of a YAML block other than copy, each qualified by the key
/// path holding it (`hero.actions[1].link: ./guide/quick-start`), so a
/// value moving to another key counts as a change.
function yamlLiterals(value: unknown, key: string): string[] {
    if (Array.isArray(value)) {
        return value.flatMap((item, i) => yamlLiterals(item, `${key}[${i}]`));
    }
    if (value !== null && typeof value === "object") {
        return Object.entries(value as Record<string, unknown>).flatMap(([name, item]) =>
            typeof item === "string" && YAML_PROSE_KEYS.has(name)
                ? []
                : yamlLiterals(item, key === "" ? name : `${key}.${name}`),
        );
    }
    return [`${key}: ${String(value)}`];
}

/// Inline code, link and image destinations, issue references such as
/// clangd#1455 in text, and the control values of YAML frontmatter,
/// anywhere below `node` except inside code blocks and comments.
function inlineLiterals(node: Nodes): string[] {
    const out = new Set<string>();
    const counts = { link: 0, image: 0 };
    const visit = (current: Nodes) => {
        switch (current.type) {
            case "code":
            case "html":
                return;
            case "inlineCode":
                out.add(`\`${current.value}\``);
                return;
            case "link":
            case "image":
                out.add(`${current.type}[${counts[current.type]}]: ${current.url}`);
                counts[current.type] += 1;
                break;
            case "definition":
                out.add(current.url);
                break;
            case "text":
                for (const match of current.value.matchAll(/[A-Za-z][\w-]*#\d+/g)) {
                    out.add(match[0]);
                }
                return;
            case "yaml": {
                let parsed: unknown;
                try {
                    parsed = parseYaml(current.value);
                } catch {
                    return;
                }
                for (const literal of yamlLiterals(parsed, "")) {
                    out.add(literal);
                }
                return;
            }
            default:
                break;
        }
        if ("children" in current) {
            for (const child of current.children) {
                visit(child);
            }
        }
    };
    visit(node);
    return [...out].sort();
}

/// Table body rows paired with the nearest later heading carrying the
/// same label: a feature page emits each capability as one status-table
/// row and one section heading from one name. Index pairs into
/// `segments`, in row order.
export function pairedLabels(
    segments: { kind: string; label: string | null }[],
): [number, number][] {
    const out: [number, number][] = [];
    const taken = new Set<number>();
    segments.forEach((row, r) => {
        if (row.kind !== "tableRow" || row.label === null) {
            return;
        }
        for (let h = r + 1; h < segments.length; h += 1) {
            const candidate = segments.at(h);
            if (candidate?.kind === "heading" && candidate.label === row.label && !taken.has(h)) {
                taken.add(h);
                out.push([r, h]);
                return;
            }
        }
    });
    return out;
}

export function splitSegments(source: string, page: string): Segment[] {
    const tree = parser.parse(source);
    const segments: Segment[] = [];
    const push = (
        node: RootContent,
        ordered: boolean,
        translatable: boolean,
        align = "",
        label: string | null = null,
    ) => {
        const range = rangeOf(node, page);
        // Take the indentation in front of a block along with it: for a
        // list item it decides how much is stripped from the lines below.
        const lineStart = source.lastIndexOf("\n", range.start - 1) + 1;
        if (source.slice(lineStart, range.start).trim() === "") {
            range.start = lineStart;
        }
        segments.push({
            ...range,
            kind: node.type,
            shape: shapeOf(node, ordered, align),
            translatable,
            verbatim: translatable ? nestedVerbatim(node, source, range.start, page) : [range],
            label,
            literals: translatable ? inlineLiterals(node) : [],
        });
    };
    for (const node of tree.children) {
        switch (node.type) {
            case "list":
                for (const item of node.children) {
                    push(item, node.ordered === true, true);
                }
                break;
            case "table":
                // The delimiter line between header and body has no row
                // node; it lands in the gap between rows and is not
                // compared. Its alignment travels with each row's shape,
                // so only dash count and padding may differ.
                node.children.forEach((row, r) => {
                    const first = row.children.at(0);
                    const label =
                        r === 0 || first === undefined ? null : phrasingOf(first, source, page);
                    push(row, false, true, tableAlign(node), label);
                });
                break;
            case "heading":
                push(node, false, true, "", phrasingOf(node, source, page));
                break;
            case "paragraph":
            case "blockquote":
                push(node, false, true);
                break;
            // VitePress home-page copy (hero text, feature cards) lives in
            // the YAML frontmatter, so the whole block is one coarse
            // translation segment.
            case "yaml":
                push(node, false, true);
                break;
            default:
                push(node, false, false);
        }
    }
    return segments;
}

export function hashSegment(text: string): string {
    return createHash("sha256").update(text, "utf8").digest("hex").slice(0, 16);
}

export function lineOf(source: string, offset: number): number {
    let line = 1;
    for (let i = 0; i < offset; i += 1) {
        if (source.charCodeAt(i) === 10) {
            line += 1;
        }
    }
    return line;
}

export function analyzeSource(source: string, page: string): SegmentInfo[] {
    return splitSegments(source, page).map((segment, position) => {
        const text = source.slice(segment.start, segment.end);
        return {
            index: position + 1,
            kind: segment.kind,
            shape: segment.shape,
            translatable: segment.translatable,
            text,
            hash: hashSegment(text),
            line: lineOf(source, segment.start),
            verbatim: segment.verbatim.map((range) => source.slice(range.start, range.end)),
            label: segment.label,
            literals: segment.literals,
        };
    });
}
