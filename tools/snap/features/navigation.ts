import * as proto from "vscode-languageserver-protocol";
import { markerPoints } from "../annotation.ts";
import { enumName, fmtRange, OffsetConverter, type Feature } from "../render.ts";
import { normalizeFileUri, yamlStr } from "../snapshot.ts";

/// The full symbol-lookup profile at each `§`-marker: definition,
/// declaration, typeDefinition, implementation, references, and one level
/// of call/type hierarchy, rendered as one block per marker with empty
/// sections dropped. Replies aggregate open sessions with index state, so
/// only the server path exists; a read-only index provider (the
/// `clice query` direction) is the precondition for an inspect twin.
///
/// Every list is sorted before rendering — reply order leaks container
/// iteration order (grouped relations come out of a DenseMap) and must
/// not be pinned.

interface Located {
    file: string;
    range: proto.Range;
    entry: string;
}

function byRange(a: proto.Range, b: proto.Range): number {
    return (
        a.start.line - b.start.line ||
        a.start.character - b.start.character ||
        a.end.line - b.end.line ||
        a.end.character - b.end.character
    );
}

function rendered(entries: Located[]): string[] {
    entries.sort(
        (a, b) =>
            (a.file < b.file ? -1 : a.file > b.file ? 1 : 0) ||
            byRange(a.range, b.range) ||
            (a.entry < b.entry ? -1 : a.entry > b.entry ? 1 : 0),
    );
    return entries.map(({ entry }) => entry);
}

function asLocations(
    reply: proto.Location | (proto.Location | proto.LocationLink)[] | null,
): proto.Location[] {
    if (reply === null) {
        return [];
    }
    return (Array.isArray(reply) ? reply : [reply]).map((entry) => {
        if (!("uri" in entry)) {
            throw new Error("clice replies plain Location arrays, not LocationLink");
        }
        return entry;
    });
}

function locationLines(locations: proto.Location[], root: string): string[] {
    return rendered(
        locations.map((location) => {
            const file = normalizeFileUri(location.uri, root);
            return {
                file,
                range: location.range,
                entry: `- { file: ${yamlStr(file)}, range: "${fmtRange(location.range)}" }`,
            };
        }),
    );
}

type HierarchyItem = proto.CallHierarchyItem | proto.TypeHierarchyItem;

/// `selection` and `detail` render only when they carry information beyond
/// `range` — today clice always sets selectionRange == range and no detail,
/// so their appearance in a snapshot IS the regression signal.
function itemFields(item: HierarchyItem, root: string): { file: string; body: string } {
    const file = normalizeFileUri(item.uri, root);
    let body =
        `name: ${yamlStr(item.name)}, kind: ${enumName(proto.SymbolKind, item.kind)}, ` +
        `file: ${yamlStr(file)}, range: "${fmtRange(item.range)}"`;
    if (byRange(item.range, item.selectionRange) !== 0) {
        body += `, selection: "${fmtRange(item.selectionRange)}"`;
    }
    if (item.detail !== undefined) {
        body += `, detail: ${yamlStr(item.detail)}`;
    }
    return { file, body };
}

function itemLines(items: HierarchyItem[], root: string): string[] {
    return rendered(
        items.map((item) => {
            const { file, body } = itemFields(item, root);
            return { file, range: item.range, entry: `- { ${body} }` };
        }),
    );
}

/// Prepare replies expand into per-item sections, so their order must be
/// pinned before iteration, not only at render time.
function sortItems<T extends HierarchyItem>(items: T[]): T[] {
    return [...items].sort(
        (a, b) => (a.uri < b.uri ? -1 : a.uri > b.uri ? 1 : 0) || byRange(a.range, b.range),
    );
}

function callLines(
    calls: { item: proto.CallHierarchyItem; fromRanges: proto.Range[] }[],
    root: string,
): string[] {
    return rendered(
        calls.map(({ item, fromRanges }) => {
            const { file, body } = itemFields(item, root);
            const ranges = [...fromRanges]
                .sort(byRange)
                .map((range) => `"${fmtRange(range)}"`)
                .join(", ");
            return {
                file,
                range: item.range,
                entry: `- { ${body}, fromRanges: [${ranges}] }`,
            };
        }),
    );
}

export const navigation: Feature = {
    shape: "point",
    fromInspect() {
        throw new Error("navigation has no inspect path; use verify: server");
    },
    async fromServer(client, uri, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const out: string[] = [];
        for (const [name, offset] of markerPoints(ctx.source)) {
            const { line, character } = map.position(offset);
            const sections: [string, string[]][] = [];

            const definition = asLocations(await client.definitionAt(uri, line, character));
            sections.push(["definition", locationLines(definition, ctx.root)]);
            const declaration = asLocations(await client.declarationAt(uri, line, character));
            sections.push(["declaration", locationLines(declaration, ctx.root)]);
            const typeDefinition = asLocations(await client.typeDefinitionAt(uri, line, character));
            sections.push(["typeDefinition", locationLines(typeDefinition, ctx.root)]);
            const implementation = asLocations(await client.implementationAt(uri, line, character));
            sections.push(["implementation", locationLines(implementation, ctx.root)]);
            const references = (await client.referencesAt(uri, line, character)) ?? [];
            sections.push(["references", locationLines(references, ctx.root)]);

            const callItems = sortItems(
                (await client.prepareCallHierarchy(uri, line, character)) ?? [],
            );
            sections.push(["callHierarchy", itemLines(callItems, ctx.root)]);
            for (const item of callItems) {
                const incoming = (await client.callHierarchyIncoming(item)) ?? [];
                sections.push([
                    "incomingCalls",
                    callLines(
                        incoming.map((call) => ({ item: call.from, fromRanges: call.fromRanges })),
                        ctx.root,
                    ),
                ]);
                const outgoing = (await client.callHierarchyOutgoing(item)) ?? [];
                sections.push([
                    "outgoingCalls",
                    callLines(
                        outgoing.map((call) => ({ item: call.to, fromRanges: call.fromRanges })),
                        ctx.root,
                    ),
                ]);
            }

            const typeItems = sortItems(
                (await client.prepareTypeHierarchy(uri, line, character)) ?? [],
            );
            sections.push(["typeHierarchy", itemLines(typeItems, ctx.root)]);
            for (const item of typeItems) {
                const supertypes = (await client.typeHierarchySupertypes(item)) ?? [];
                sections.push(["supertypes", itemLines(supertypes, ctx.root)]);
                const subtypes = (await client.typeHierarchySubtypes(item)) ?? [];
                sections.push(["subtypes", itemLines(subtypes, ctx.root)]);
            }

            if (out.length > 0) {
                out.push("");
            }
            const present = sections.filter(([, lines]) => lines.length > 0);
            if (present.length === 0) {
                out.push(`${name}: none`);
                continue;
            }
            out.push(`${name}:`);
            for (const [label, lines] of present) {
                out.push(`  ${label}:`);
                out.push(...lines.map((entryLine) => `    ${entryLine}`));
            }
        }
        return out;
    },
};
