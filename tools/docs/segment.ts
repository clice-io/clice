/// Markdown segmentation shared by the translation tools. A page is split
/// into top-level segments; translatable ones (headings, paragraphs,
/// blockquotes, list items, table rows, YAML frontmatter) carry prose,
/// everything else (code blocks, HTML comments, ...) is verbatim. The
/// en↔zh contract in translate.ts compares these sequences and its
/// translate mode feeds the translatable ones to a model.

import { createHash } from "node:crypto";
import remarkFrontmatter from "remark-frontmatter";
import remarkGfm from "remark-gfm";
import remarkParse from "remark-parse";
import { unified } from "unified";
import type { Node, Point } from "unist";

export interface Segment {
    start: number;
    end: number;
    kind: string;
    translatable: boolean;
}

/// A segment enriched with everything comparisons need. `index` is the
/// 1-based position in the full segment sequence (translatable and
/// verbatim alike), so messages number segments consistently.
export interface SegmentInfo {
    index: number;
    kind: string;
    translatable: boolean;
    text: string;
    hash: string;
    line: number;
}

export const parser = unified().use(remarkParse).use(remarkGfm).use(remarkFrontmatter);

function offsetOf(point: Point | undefined, page: string): number {
    if (point?.offset === undefined) {
        throw new Error(`${page}: parser produced a node without source offsets`);
    }
    return point.offset;
}

export function splitSegments(source: string, page: string): Segment[] {
    const tree = parser.parse(source);
    const segments: Segment[] = [];
    const push = (node: Node, translatable: boolean) => {
        segments.push({
            start: offsetOf(node.position?.start, page),
            end: offsetOf(node.position?.end, page),
            kind: node.type,
            translatable,
        });
    };
    for (const node of tree.children) {
        switch (node.type) {
            case "list":
                for (const item of node.children) {
                    push(item, true);
                }
                break;
            case "table":
                // The delimiter line between header and body has no row
                // node; it lands in the gap between rows and is not
                // compared, so the two sides may align columns differently.
                for (const row of node.children) {
                    push(row, true);
                }
                break;
            case "heading":
            case "paragraph":
            case "blockquote":
                push(node, true);
                break;
            // VitePress home-page copy (hero text, feature cards) lives in
            // the YAML frontmatter, so the whole block is one coarse
            // translation segment.
            case "yaml":
                push(node, true);
                break;
            default:
                push(node, false);
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
            translatable: segment.translatable,
            text,
            hash: hashSegment(text),
            line: lineOf(source, segment.start),
        };
    });
}
