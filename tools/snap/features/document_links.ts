import { fmtRange, OffsetConverter, type Feature } from "../render.ts";
import { normalizeFilePath, normalizeFileUri, yamlStr } from "../snapshot.ts";

interface LinkEntry {
    range: string;
    /// `${WS}`-relative target, already validated and normalized.
    target: string;
}

interface RawDocumentLink {
    range: { begin: number; end: number };
    target: string;
}

function formatDocumentLinks(links: LinkEntry[]): string[] {
    return links.map((link) => `- { range: "${link.range}", target: ${yamlStr(link.target)} }`);
}

export const documentLinks: Feature = {
    shape: "document",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        return formatDocumentLinks(
            (entry.result as RawDocumentLink[]).map((link) => {
                const start = map.position(link.range.begin);
                const end = map.position(link.range.end);
                return {
                    range: `${start.line}:${start.character}-${end.line}:${end.character}`,
                    target: normalizeFilePath(link.target, ctx.root),
                };
            }),
        );
    },
    async fromServer(client, uri, ctx) {
        const links = (await client.documentLinks(uri)) ?? [];
        return formatDocumentLinks(
            links.map((link) => {
                if (link.target === undefined) {
                    throw new Error("clice always resolves link targets");
                }
                const target = normalizeFileUri(link.target, ctx.root);
                // The tooltip mirrors the target as a raw absolute path; it
                // is validated on every fixture instead of pinned, so shared
                // snapshots stay byte-identical with the inspect path (whose
                // payload has no tooltip).
                if (link.tooltip === undefined) {
                    throw new Error(`link ${link.target} carries no tooltip`);
                }
                if (normalizeFilePath(link.tooltip, ctx.root) !== target) {
                    throw new Error(
                        `tooltip ${link.tooltip} does not resolve to the link target ${link.target}`,
                    );
                }
                return { range: fmtRange(link.range), target };
            }),
        );
    },
};
