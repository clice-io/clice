/// Wire-side snap driver: replay a fixture through a real server over LSP
/// and pin the reply. Together with standalone.ts this is the whole snap
/// domain — the integration suite plays no part in snapshots. For a
/// `snap: shared` fixture both drivers must render byte-identically into
/// one colocated file; a mismatch here is a real divergence between the
/// server pipeline and the direct feature call, never something to paper
/// over with UPDATE_SNAPSHOTS.

import * as fs from "node:fs";
import * as path from "node:path";
import type { CliceClient } from "../client/client.ts";
import type { Workspace } from "../client/workspace.ts";
import { DATA_DIR, SNAPSHOTS_DIR } from "../compile_commands.ts";
import { parseAnnotations } from "./annotation.ts";
import {
    presentDocumentLinks,
    presentDocumentSymbols,
    presentFoldingRanges,
    presentInlayHints,
    presentSemanticTokens,
    type Presenter,
} from "./presenters.ts";
import { fixtureFrontmatter, SnapshotContext } from "./snapshot.ts";
import type { SnapCorpus, SnapFixture } from "./standalone.ts";

export interface WireSession {
    client: CliceClient;
    workspace: Workspace;
}

const WIRE_PRESENTERS: Record<string, Presenter> = {
    folding_range: presentFoldingRanges,
    semantic_tokens: presentSemanticTokens,
};

/// The wire presenter for a snap corpus. A corpus without one would be
/// silently skipped on this side while the standalone driver still runs
/// it, so that is an error.
export function wirePresenter(feature: string): Presenter {
    const present = WIRE_PRESENTERS[feature];
    if (!present) {
        throw new Error(`no wire presenter registered for tests/snap/${feature}`);
    }
    return present;
}

/// Replay one snap fixture through the session's server and compare the
/// reply against the colocated snapshot. Shared snapshot bodies are owned
/// by the standalone driver; even under UPDATE_SNAPSHOTS the wire side
/// must never overwrite one — that would paper over the exact cross-path
/// divergence the shared file exists to catch.
export async function checkWireSnapFixture(
    { client, workspace }: WireSession,
    corpus: SnapCorpus,
    fixture: SnapFixture,
): Promise<void> {
    const present = wirePresenter(corpus.feature);
    const source = parseAnnotations(fixture.content);
    const [uri] = await client.openAndWait(fixture.rel, 60_000, { text: source.content });
    const body = await present(client, uri, source, workspace);
    client.close(uri);

    const shared = fixture.meta.snap === "shared";
    const snapshots = shared
        ? new SnapshotContext(corpus.corpus, { colocated: true, update: false })
        : new SnapshotContext(corpus.corpus, { colocated: true });
    snapshots.check(fixture.rel, body.join("\n"), shared ? "" : "wire");
}

/// Corpora under tests/data that have not migrated to tests/snap yet; their
/// wire replies pin under tests/snapshots/integration/<feature>/.
const LEGACY_FEATURES: Record<string, Presenter> = {
    document_links: presentDocumentLinks,
    document_symbol: presentDocumentSymbols,
    inlay_hint: presentInlayHints,
};

export interface LegacyCorpus {
    feature: string;
    corpus: string;
    fixtures: string[];
}

export function legacyCorpora(): LegacyCorpus[] {
    return Object.keys(LEGACY_FEATURES).map((feature) => {
        const corpus = path.join(DATA_DIR, feature);
        const fixtures = fs
            .readdirSync(corpus, { recursive: true, encoding: "utf8" })
            .filter((name) => name.endsWith(".cpp"))
            .sort()
            .map((name) => name.split(path.sep).join("/"));
        return { feature, corpus, fixtures };
    });
}

/// Legacy behavior: `status: unsupported` fixtures pin a literal
/// UNSUPPORTED marker instead of being replayed.
export async function checkLegacyWireFixture(
    session: () => Promise<WireSession>,
    { feature, corpus }: LegacyCorpus,
    rel: string,
): Promise<void> {
    const present = LEGACY_FEATURES[feature];
    if (!present) {
        throw new Error(`no legacy presenter registered for ${feature}`);
    }
    const snapshots = new SnapshotContext(path.join(SNAPSHOTS_DIR, "integration", feature));

    const content = fs.readFileSync(path.join(corpus, rel), "utf8");
    if (fixtureFrontmatter(content, "status") === "unsupported") {
        snapshots.check(rel, "UNSUPPORTED");
        return;
    }

    const { client, workspace } = await session();
    const source = parseAnnotations(content);
    const [uri] = await client.openAndWait(rel, 60_000, { text: source.content });
    const body = await present(client, uri, source, workspace);
    client.close(uri);
    snapshots.check(rel, body.join("\n"));
}
