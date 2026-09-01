/// Markdown segmentation shared by the translation tools. A page is split
/// into top-level segments; translatable ones (headings, paragraphs,
/// blockquotes, list items, table rows, YAML frontmatter) carry prose,
/// everything else (code blocks, HTML comments, ...) is verbatim. The
/// en↔zh contract in translate.ts compares these sequences and its
/// translate mode feeds the translatable ones to a model.

import { createHash } from "node:crypto";
import type { List, Nodes, RootContent } from "mdast";
import remarkFrontmatter from "remark-frontmatter";
import remarkGfm from "remark-gfm";
import remarkParse from "remark-parse";
import { unified } from "unified";
import type { Point } from "unist";

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

function shapeOf(node: RootContent, parent: List | null): string {
    switch (node.type) {
        case "heading":
            return `heading:${node.depth}`;
        case "listItem":
            return parent?.ordered === true ? "listItem:ordered" : "listItem";
        case "tableRow":
            return `tableRow:${node.children.length}`;
        default:
            return node.type;
    }
}

/// Fenced or indented code blocks anywhere below `node` (inline code is
/// prose and may be reflowed by a translation).
function nestedCode(node: Nodes, page: string): Range[] {
    const out: Range[] = [];
    const visit = (current: Nodes) => {
        if (current.type === "code") {
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
    const push = (node: RootContent, parent: List | null, translatable: boolean) => {
        const range = rangeOf(node, page);
        segments.push({
            ...range,
            kind: node.type,
            shape: shapeOf(node, parent),
            translatable,
            verbatim: translatable ? nestedCode(node, page) : [range],
        });
    };
    for (const node of tree.children) {
        switch (node.type) {
            case "list":
                for (const item of node.children) {
                    push(item, node, true);
                }
                break;
            case "table":
                // The delimiter line between header and body has no row
                // node; it lands in the gap between rows and is not
                // compared, so the two sides may align columns differently.
                for (const row of node.children) {
                    push(row, null, true);
                }
                break;
            case "heading":
            case "paragraph":
            case "blockquote":
                push(node, null, true);
                break;
            // VitePress home-page copy (hero text, feature cards) lives in
            // the YAML frontmatter, so the whole block is one coarse
            // translation segment.
            case "yaml":
                push(node, null, true);
                break;
            default:
                push(node, null, false);
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
