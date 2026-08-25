import * as proto from "vscode-languageserver-protocol";
import { enumName, fmtRange, type Feature } from "../render.ts";
import { normalizeFileUri, yamlStr } from "../snapshot.ts";

/// workspace/symbol snapshots. The request is query-driven, not
/// position-driven, so instead of `§` markers a fixture declares its
/// queries as comment lines in the source — they double as part of the
/// rendered doc example:
///
///     // query: Widget
///
/// Each `// query:` line pins one reply block. Replies aggregate open
/// sessions with project-index state, so only the server path exists;
/// the server driver opens every source of the fixture's unit, which is
/// what places their symbols in the search space without background
/// indexing. That default cannot witness project-wide search: an open
/// sibling's hits prove nothing about closed files. A fixture claiming
/// them keeps the target file closed — a markerless header sibling is
/// never opened — sets `indexing: true`, and names each symbol that must
/// arrive from the background index in an `// indexed:` line (inside the
/// `// snap:` block, so docs drop it); queries wait for those symbols
/// before pinning, turning a lost project-index path into a timeout
/// failure instead of a green snapshot. Results are sorted before
/// rendering — reply order leaks container iteration order and the
/// server caps result counts, so fixtures must stay small enough that no
/// query ever hits the cap.

function byRange(a: proto.Range, b: proto.Range): number {
    return (
        a.start.line - b.start.line ||
        a.start.character - b.start.character ||
        a.end.line - b.end.line ||
        a.end.character - b.end.character
    );
}

/// The `<directive>:` comment lines of a fixture, in source order.
function directiveLines(stripped: Buffer, directive: string): string[] {
    const out: string[] = [];
    for (const line of stripped.toString("utf8").split("\n")) {
        const trimmed = line.trim();
        if (!trimmed.startsWith(`// ${directive}:`)) {
            continue;
        }
        const value = trimmed.slice(`// ${directive}:`.length).trim();
        if (value === "") {
            throw new Error(`empty '// ${directive}:' line in workspace_symbol fixture`);
        }
        if (out.includes(value)) {
            throw new Error(`duplicate '// ${directive}:' line '${value}'`);
        }
        out.push(value);
    }
    return out;
}

export const workspaceSymbol: Feature = {
    shape: "document",
    fromInspect() {
        throw new Error("workspace symbol has no inspect path; use verify: server");
    },
    async fromServer(client, uri, ctx) {
        const queries = directiveLines(ctx.stripped, "query");
        if (queries.length === 0) {
            throw new Error("workspace_symbol fixture declares no '// query:' lines");
        }
        const indexed = directiveLines(ctx.stripped, "indexed");
        if (indexed.length > 0 && ctx.indexing !== true) {
            throw new Error("'// indexed:' lines require 'indexing: true' in the fixture meta");
        }
        if (indexed.length === 0 && ctx.indexing === true) {
            throw new Error(
                "an 'indexing: true' fixture must name its '// indexed:' symbols; " +
                    "without the wait gate every query races the background index",
            );
        }
        for (const name of indexed) {
            if (!(await client.waitForIndex(uri, name))) {
                throw new Error(
                    `symbol '${name}' never arrived from the background index; ` +
                        "is its file both closed and listed in the compilation database?",
                );
            }
        }

        const out: string[] = [];
        for (const query of queries) {
            const reply = (await client.workspaceSymbols(query)) ?? [];
            // The server truncates at 100 results during unordered index
            // iteration; a reply that size is an arbitrary subset.
            if (reply.length >= 100) {
                throw new Error(`query '${query}' hits the server result cap; narrow the fixture`);
            }
            const entries = reply.map((symbol) => {
                const location = symbol.location;
                if (!("range" in location)) {
                    throw new Error("clice replies SymbolInformation with full locations");
                }
                const file = normalizeFileUri(location.uri, ctx.root);
                let body =
                    `name: ${yamlStr(symbol.name)}, ` +
                    `kind: ${enumName(proto.SymbolKind, symbol.kind)}, ` +
                    `file: ${yamlStr(file)}, range: "${fmtRange(location.range)}"`;
                // Absent today — its appearance in a snapshot IS the
                // regression signal.
                if (symbol.containerName !== undefined) {
                    body += `, container: ${yamlStr(symbol.containerName)}`;
                }
                return { file, range: location.range, body };
            });
            entries.sort(
                (a, b) =>
                    (a.file < b.file ? -1 : a.file > b.file ? 1 : 0) ||
                    byRange(a.range, b.range) ||
                    (a.body < b.body ? -1 : a.body > b.body ? 1 : 0),
            );
            if (out.length > 0) {
                out.push("");
            }
            if (entries.length === 0) {
                out.push(`${yamlStr(query)}: none`);
                continue;
            }
            out.push(`${yamlStr(query)}:`);
            out.push(...entries.map(({ body }) => `  - { ${body} }`));
        }
        return out;
    },
};
