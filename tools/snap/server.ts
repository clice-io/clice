/// Server-path snap driver: replay a fixture through a real server over
/// LSP and pin the reply. Together with the inspect driver this is the
/// whole snap domain — the integration suite plays no part in snapshots.
/// For a `snap: shared` fixture both drivers must render byte-identically
/// into one colocated file; a mismatch here is a real divergence between
/// the server pipeline and the direct feature call, never something to
/// paper over with UPDATE_SNAPSHOTS.
///
/// Every fixture runs in its own materialized throwaway workspace (see
/// materializeFixture): sources arrive on disk already stripped, so disk
/// and didOpen agree, background indexing sees the same bytes, and no
/// fixture shares state with another — no corpus workspace, no lock.

import type { CliceClient } from "../client/client.ts";
import type { SessionFactory } from "../client/session.ts";
import {
    HEADER,
    materializeFixture,
    type FixtureFile,
    type SnapCorpus,
    type SnapFixture,
} from "./corpus.ts";
import { feature, participates } from "./registry.ts";
import { abBlocks, fileSections } from "./render.ts";
import { SnapshotContext } from "./snapshot.ts";

/// Replay one snap fixture through a real server and compare the reply
/// against the colocated snapshot. Shared snapshot bodies are owned by the
/// inspect driver; even under UPDATE_SNAPSHOTS this side must never
/// overwrite one — that would paper over the exact cross-path divergence
/// the shared file exists to catch. `snap: separate` and `verify: server`
/// fixtures are pinned from this side, so their files stay updatable.
///
/// A `config:` fixture replays twice: the default half on a plainly
/// initialized server, the configured half on a second server whose
/// initializationOptions carry the overlay under the feature's config
/// section. The second server reuses the workspace (and its .clice cache)
/// the default half just used — fine while config fixtures pin replies
/// recomputed from source on every request, but a future config fixture
/// depending on project-index state would need a cache clear between the
/// halves.
export async function checkServerSnapFixture(
    session: SessionFactory,
    corpus: SnapCorpus,
    fixture: SnapFixture,
): Promise<void> {
    const { shape, fromServer } = feature(corpus.feature);
    const workspace = session.tmpdir();
    materializeFixture(corpus, fixture, workspace.root);

    const initializationOptions = (configured: boolean): Record<string, unknown> => {
        // A throwaway replay workspace has no edit storm to batch, so an
        // indexing fixture skips the idle window instead of stalling every
        // run on it.
        const options: Record<string, unknown> = {
            project: fixture.meta.indexing
                ? { enable_indexing: true, idle_timeout_ms: 10 }
                : { enable_indexing: false },
        };
        if (configured && fixture.meta.config !== undefined) {
            options[corpus.configSection] = JSON.parse(fixture.meta.config) as Record<
                string,
                unknown
            >;
        }
        return options;
    };

    const present = async (client: CliceClient): Promise<string[]> => {
        // Sibling sources open before the entry: a module interface must be
        // scanned before an import of it resolves. Headers open only when
        // they participate — a support header may be valid only through its
        // includer, and the inspect path never compiles one standalone
        // either.
        const opened: [FixtureFile, string][] = [];
        const errors: string[] = [];
        const ordered = [
            ...fixture.files.filter((file) => file.rel !== fixture.rel),
            ...fixture.files.filter((file) => file.rel === fixture.rel),
        ].filter(
            (file) =>
                !HEADER.test(file.rel) ||
                participates(shape, file.source, file.rel === fixture.rel),
        );
        for (const file of ordered) {
            const [uri] = await client.openAndWait(file.rel, 60_000);
            opened.push([file, uri]);
            errors.push(
                ...client
                    .errors(uri)
                    .map(({ message }) =>
                        typeof message === "string" ? message : JSON.stringify(message),
                    ),
            );
        }
        // The inspect driver owns the strict two-way diagnostics gate; here
        // only unexpected errors fail — the preamble split may legitimately
        // shift where an expected-broken fixture's diagnostics surface. A
        // server-only fixture never runs the inspect gate, so the stale
        // `diagnostics: expected` direction must be enforced here.
        if (errors.length > 0 && !fixture.meta.diagnostics) {
            throw new Error(
                `${corpus.feature}/${fixture.rel}: fixture does not compile cleanly:\n  ` +
                    errors.join("\n  "),
            );
        }
        if (errors.length === 0 && fixture.meta.diagnostics && fixture.meta.verify === "server") {
            throw new Error(
                `${corpus.feature}/${fixture.rel}: diagnostics: expected, ` +
                    "but the fixture compiled cleanly",
            );
        }

        // Sections render in rel order, like the inspect driver's.
        opened.sort(([a], [b]) => (a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0));
        const sections: [string, string[]][] = [];
        for (const [file, uri] of opened) {
            if (!participates(shape, file.source, file.rel === fixture.rel)) {
                continue;
            }
            const label = fixture.unit === "" ? file.rel : file.rel.slice(fixture.unit.length + 1);
            const stripped = Buffer.from(file.source.content);
            sections.push([
                label,
                await fromServer(client, uri, {
                    source: file.source,
                    stripped,
                    root: workspace.root,
                    indexing: fixture.meta.indexing,
                }),
            ]);
        }
        if (sections.length === 0) {
            throw new Error(`${corpus.feature}/${fixture.rel}: no file carries ${shape} markers`);
        }
        return fileSections(sections);
    };

    const client = session.spawn(workspace);
    await client.initialize(workspace, { initializationOptions: initializationOptions(false) });
    let body = await present(client);
    if (fixture.meta.config !== undefined) {
        await client.shutdown();
        const configured = session.spawn(workspace);
        await configured.initialize(workspace, {
            initializationOptions: initializationOptions(true),
        });
        body = abBlocks(body, await present(configured));
    }

    const shared = fixture.meta.verify === "both" && fixture.meta.snap === "shared";
    const snapshots = shared
        ? new SnapshotContext(corpus.corpus, { colocated: true, update: false })
        : new SnapshotContext(corpus.corpus, { colocated: true });
    const variant =
        fixture.meta.verify === "both" && fixture.meta.snap === "separate" ? "server" : "";
    snapshots.check(fixture.rel, body.join("\n"), variant);
}
