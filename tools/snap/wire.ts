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
import type { SessionFactory } from "../client/session.ts";
import type { Workspace } from "../client/workspace.ts";
import { parseAnnotations, type AnnotatedSource } from "./annotation.ts";
import {
    abBlocks,
    presentCodeCompletion,
    presentDocumentLinks,
    presentDocumentSymbols,
    presentFoldingRanges,
    presentHover,
    presentInlayHints,
    presentSemanticTokens,
    presentSignatureHelp,
    type Presenter,
} from "./presenters.ts";
import { SnapshotContext } from "./snapshot.ts";
import type { SnapCorpus, SnapFixture } from "./standalone.ts";

const WIRE_PRESENTERS: Record<string, Presenter> = {
    code_completion: presentCodeCompletion,
    document_links: presentDocumentLinks,
    document_symbol: presentDocumentSymbols,
    folding_range: presentFoldingRanges,
    hover: presentHover,
    inlay_hint: presentInlayHints,
    semantic_tokens: presentSemanticTokens,
    signature_help: presentSignatureHelp,
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

/// The sibling sources of a multi-file fixture (a subdirectory entered
/// through its main.cpp), as paths relative to the unit workspace,
/// sorted for a deterministic open order. A plain single-file fixture
/// has none.
function fixtureSiblings(corpus: string, rel: string): string[] {
    if (!rel.endsWith("/main.cpp")) {
        return [];
    }
    const dir = rel.slice(0, -"/main.cpp".length);
    return fs
        .readdirSync(path.join(corpus, dir))
        .filter((name) => name !== "main.cpp" && /\.(cppm|cpp|cc|c)$/.test(name))
        .sort();
}

/// One presenter pass against one server: open a multi-file fixture's
/// sibling sources first (a module interface must be scanned before
/// import completion lists it), then the fixture entry itself.
async function presentOn(
    client: CliceClient,
    workspace: Workspace,
    present: Presenter,
    entryRel: string,
    source: AnnotatedSource,
    siblings: string[],
): Promise<string[]> {
    for (const rel of siblings) {
        await client.openAndWait(rel, 60_000);
    }
    const [uri] = await client.openAndWait(entryRel, 60_000, { text: source.content });
    const body = await present(client, uri, source, workspace);
    client.close(uri);
    return body;
}

/// Replay one snap fixture through a real server and compare the reply
/// against the colocated snapshot. Shared snapshot bodies are owned by the
/// standalone driver; even under UPDATE_SNAPSHOTS the wire side must never
/// overwrite one — that would paper over the exact cross-path divergence
/// the shared file exists to catch. `snap: separate` and `snap: wire`
/// fixtures are pinned from this side, so those files stay updatable.
///
/// A `config:` fixture replays twice: the default half on a plainly
/// initialized server, the configured half on a second server whose
/// initializationOptions carry the overlay under the feature's config
/// section — the servers run strictly one after the other on the corpus
/// workspace, inside the same workspace lock.
export async function checkWireSnapFixture(
    session: SessionFactory,
    corpus: SnapCorpus,
    fixture: SnapFixture,
): Promise<void> {
    const present = wirePresenter(corpus.feature);
    const source = parseAnnotations(fixture.content);
    // A multi-file fixture's subdirectory IS its workspace: the session
    // initializes there, on the unit's own CDB, so units stay isolated
    // from the corpus and from each other (module names, for one, cannot
    // collide across fixtures).
    const unitDir = fixture.rel.endsWith("/main.cpp")
        ? fixture.rel.slice(0, -"/main.cpp".length)
        : null;
    const sessionName =
        unitDir === null ? `snap/${corpus.feature}` : `snap/${corpus.feature}/${unitDir}`;
    const entryRel = unitDir === null ? fixture.rel : "main.cpp";
    const siblings = fixtureSiblings(corpus.corpus, fixture.rel);
    const { client, workspace } = await session(sessionName);

    let body = await presentOn(client, workspace, present, entryRel, source, siblings);
    if (fixture.meta.config !== undefined) {
        await client.shutdown();
        // The second server reuses the workspace (and its .clice cache)
        // the default half just used — fine while config fixtures pin
        // completion replies recomputed from source on every request, but
        // a future config fixture depending on project-index state would
        // need a cache clear between the halves.
        const configured = session.spawn(workspace);
        await configured.initialize(workspace, {
            initializationOptions: {
                [corpus.feature]: JSON.parse(fixture.meta.config) as Record<string, unknown>,
            },
        });
        body = abBlocks(
            body,
            await presentOn(configured, workspace, present, entryRel, source, siblings),
        );
    }

    const shared = fixture.meta.snap === "shared";
    const snapshots = shared
        ? new SnapshotContext(corpus.corpus, { colocated: true, update: false })
        : new SnapshotContext(corpus.corpus, { colocated: true });
    snapshots.check(fixture.rel, body.join("\n"), fixture.meta.snap === "separate" ? "wire" : "");
}
