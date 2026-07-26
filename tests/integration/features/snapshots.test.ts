/// Wire-level snapshot tests over the fixture corpora shared with the unit
/// snapshot glob; results are pinned under tests/snapshots/integration/.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "@clice/tools/annotation";
import { DATA_DIR, SNAPSHOTS_DIR } from "@clice/tools/compile-commands";
import { test } from "../../fixtures.ts";
import {
    presentDocumentLinks,
    presentDocumentSymbols,
    presentFoldingRanges,
    presentInlayHints,
    presentSemanticTokens,
    type Presenter,
} from "@clice/tools/presenters";
import { fixtureFrontmatter, SnapshotContext } from "@clice/tools/snapshot";

const FEATURES: Record<string, Presenter> = {
    document_links: presentDocumentLinks,
    folding_range: presentFoldingRanges,
    document_symbol: presentDocumentSymbols,
    semantic_tokens: presentSemanticTokens,
    inlay_hint: presentInlayHints,
};

for (const [feature, present] of Object.entries(FEATURES)) {
    const corpus = path.join(DATA_DIR, feature);
    const fixtures = fs
        .readdirSync(corpus, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".cpp"))
        .sort()
        .map((name) => name.split(path.sep).join("/"));

    for (const rel of fixtures) {
        test(`${feature}/${rel}`, async ({ session }) => {
            const snapshots = new SnapshotContext(path.join(SNAPSHOTS_DIR, "integration", feature));

            const content = fs.readFileSync(path.join(corpus, rel), "utf8");
            if (fixtureFrontmatter(content, "status") === "unsupported") {
                snapshots.check(rel, "UNSUPPORTED");
                return;
            }

            const { client, workspace } = await session(feature);
            const source = parseAnnotations(content);
            const [uri] = await client.openAndWait(rel, 60_000, {
                text: source.content,
            });
            const body = await present(client, uri, source, workspace);
            client.close(uri);
            snapshots.check(rel, body.join("\n"));
        });
    }
}
