/// Wire-level snapshot tests over the fixture corpora.
///
/// Corpora under tests/snap/ are shared with the standalone suite
/// (tests/snap/snap.test.ts): a `snap: shared` fixture pins both paths in
/// one colocated snapshot, so a mismatch here is a real divergence between
/// the server pipeline and the direct feature call — investigate it, never
/// UPDATE_SNAPSHOTS over it. `snap: separate` fixtures pin the wire reply
/// in their own `.wire.snap.yml`. Legacy corpora under tests/data/ still
/// pin under tests/snapshots/integration/.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "@clice/tools/annotation";
import { DATA_DIR, SNAP_DIR, SNAPSHOTS_DIR } from "@clice/tools/compile-commands";
import { parseFixtureMeta } from "@clice/tools/inspect";
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

const LEGACY_FEATURES: Record<string, Presenter> = {
    document_links: presentDocumentLinks,
    document_symbol: presentDocumentSymbols,
    inlay_hint: presentInlayHints,
};

const SNAP_FEATURES: Record<string, Presenter> = {
    folding_range: presentFoldingRanges,
    semantic_tokens: presentSemanticTokens,
};

function fixturesIn(corpus: string): string[] {
    return fs
        .readdirSync(corpus, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".cpp"))
        .sort()
        .map((name) => name.split(path.sep).join("/"));
}

for (const [feature, present] of Object.entries(LEGACY_FEATURES)) {
    const corpus = path.join(DATA_DIR, feature);
    for (const rel of fixturesIn(corpus)) {
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

for (const [feature, present] of Object.entries(SNAP_FEATURES)) {
    const corpus = path.join(SNAP_DIR, feature);
    for (const rel of fixturesIn(corpus)) {
        const content = fs.readFileSync(path.join(corpus, rel), "utf8");
        const meta = parseFixtureMeta(content, `${feature}/${rel}`);

        test.skipIf(meta.status === "unsupported" || meta.snap === "skip")(
            `${feature}/${rel}`,
            async ({ session }) => {
                const { client, workspace } = await session(`snap/${feature}`);
                const source = parseAnnotations(content);
                const [uri] = await client.openAndWait(rel, 60_000, {
                    text: source.content,
                });
                const body = await present(client, uri, source, workspace);
                client.close(uri);
                new SnapshotContext(corpus, { colocated: true }).check(
                    rel,
                    body.join("\n"),
                    meta.snap === "shared" ? "" : "wire",
                );
            },
        );
    }
}
