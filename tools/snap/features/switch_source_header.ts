import { URI, Utils } from "vscode-uri";
import { SETTLE_TIME, sleep } from "../../client/client.ts";
import { directiveLines, type Feature } from "../render.ts";
import { normalizeFileUri, yamlStr } from "../snapshot.ts";

/// clice/counterparts snapshots. The request names a file, not a position,
/// so a fixture declares the files to ask about as comment lines in its
/// entry source, relative to it:
///
///     // switch: widget.h
///
/// Each line pins one reply block, candidates in the server's order — the
/// ranking is what the feature answers. Replies read the build and the
/// project index, so only the server path exists. Shared declarations of
/// closed files come from the background index: an `indexing: true` fixture
/// waits for it to go idle before asking.

const INDEX_TIMEOUT_MS = 60_000;

export const switchSourceHeader: Feature = {
    shape: "document",
    fromInspect() {
        throw new Error("switch source/header has no inspect path; use verify: server");
    },
    async fromServer(client, uri, ctx) {
        const files = directiveLines(ctx.stripped, "switch");
        if (files.length === 0) {
            throw new Error("switch_source_header fixture declares no '// switch:' lines");
        }
        if (ctx.indexing === true) {
            const deadline = Date.now() + INDEX_TIMEOUT_MS;
            while (!(await client.stats()).indexIdle) {
                if (Date.now() > deadline) {
                    throw new Error("background indexing never went idle");
                }
                await sleep(SETTLE_TIME);
            }
        }

        const directory = Utils.dirname(URI.parse(uri));
        const out: string[] = [];
        for (const file of files) {
            const reply = await client.counterparts(Utils.joinPath(directory, file).toString());
            if (out.length > 0) {
                out.push("");
            }
            if (reply.candidates.length === 0) {
                out.push(`${yamlStr(file)}: none`);
                continue;
            }
            out.push(`${yamlStr(file)}:`);
            if (reply.preferred !== null) {
                out.push(`  preferred: ${yamlStr(normalizeFileUri(reply.preferred, ctx.root))}`);
            }
            out.push("  candidates:");
            for (const candidate of reply.candidates) {
                const reasons = candidate.reasons.map((reason) => yamlStr(reason)).join(", ");
                out.push(
                    `    - { file: ${yamlStr(normalizeFileUri(candidate.uri, ctx.root))}, ` +
                        `reasons: [${reasons}] }`,
                );
            }
        }
        return out;
    },
};
