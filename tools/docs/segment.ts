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
    /// blocks nested inside it (a snap example under its checklist item).
    verbatim: Range[];
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

/// Code blocks and HTML comments anywhere below `node` (inline code and
/// other inline HTML are prose and may be reflowed by a translation). A
/// code range starts at the beginning of the opening fence's line, so the
/// list or blockquote prefix in front of the fence is compared too — it
/// decides how much indentation is stripped from the block's lines.
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

export function splitSegments(source: string, page: string): Segment[] {
    const tree = parser.parse(source);
    const segments: Segment[] = [];
    const push = (node: RootContent, ordered: boolean, translatable: boolean, align = "") => {
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
                for (const row of node.children) {
                    push(row, false, true, tableAlign(node));
                }
                break;
            case "heading":
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
        };
    });
}
