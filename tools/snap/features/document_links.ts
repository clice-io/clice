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
                return {
                    range: fmtRange(link.range),
                    target: normalizeFileUri(link.target, ctx.root),
                };
            }),
        );
    },
};
