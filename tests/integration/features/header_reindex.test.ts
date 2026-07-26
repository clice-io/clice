/// Saving a header must reindex the closed TUs that include it.

import * as fs from "node:fs";
import * as path from "node:path";
import { MTIME_GRANULARITY, referenceUris, sleep, waitForReference } from "../../tools/checks.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { expect, test } from "../../tools/fixtures.ts";

const HEADER_V1 = `#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

// Retargets the closed TU's call from alpha() to beta() without touching
// the TU itself — only a reindex against the new header can see it.
const HEADER_V2 = `#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const CLOSED_TU = '#include "header.h"\nint use_target() { return TARGET(); }\n';

test("header save reindexes dependents", async ({ session }) => {
    const tmp = session.tmpdir();
    // The on-disk bytes are kept identical to the didChange text below:
    // after a save the buffer and the disk must agree, as they do for a
    // real editor. Index navigation on an open file resolves positions
    // against the buffer while shards index the disk content, so a CRLF
    // translation here would make every lookup miss on Windows.
    fs.writeFileSync(path.join(tmp, "header.h"), HEADER_V1);
    fs.writeFileSync(path.join(tmp, "closed.cpp"), CLOSED_TU);
    writeCdb(tmp, ["closed.cpp"]);
    const client = session.spawn(tmp);
    await client.initialize(tmp);

    const headerUri = client.pathToUri(path.join(tmp, "header.h"));
    const closedUri = client.pathToUri(path.join(tmp, "closed.cpp"));
    await client.openAndWait(path.join(tmp, "header.h"));

    // Initial background index: the closed TU's call resolves to alpha.
    expect(
        await waitForReference(client, headerUri, 1, 11, closedUri),
        "initial index never produced the closed TU's alpha reference",
    ).toBe(true);
    expect(await referenceUris(client, headerUri, 2, 11)).not.toContain(closedUri);

    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(tmp, "header.h"), HEADER_V2);
    client.change(headerUri, 2, HEADER_V2);
    client.save(headerUri);

    // The closed TU is reindexed against the saved header: its call now
    // references beta, and the stale alpha reference is gone.
    expect(
        await waitForReference(client, headerUri, 2, 11, closedUri),
        "closed TU was not reindexed after the header save",
    ).toBe(true);
    expect(await referenceUris(client, headerUri, 1, 11)).not.toContain(closedUri);
});

test("divergent save follows disk", async ({ session }) => {
    const tmp = session.tmpdir();
    fs.writeFileSync(path.join(tmp, "header.h"), HEADER_V1);
    fs.writeFileSync(path.join(tmp, "closed.cpp"), CLOSED_TU);
    writeCdb(tmp, ["closed.cpp"]);
    const client = session.spawn(tmp);
    await client.initialize(tmp);

    const headerUri = client.pathToUri(path.join(tmp, "header.h"));
    const closedUri = client.pathToUri(path.join(tmp, "closed.cpp"));
    await client.openAndWait(path.join(tmp, "header.h"));

    expect(
        await waitForReference(client, headerUri, 1, 11, closedUri),
        "initial index never produced the closed TU's alpha reference",
    ).toBe(true);

    // A save hook rewrote the file as the save landed: the disk holds V2
    // while the buffer still holds V1 and no didChange is ever sent.
    // (alpha/beta keep their positions across versions, so buffer-resolved
    // lookups on the open header stay valid.)
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(tmp, "header.h"), HEADER_V2);
    client.save(headerUri);

    // Dependents must follow the disk truth, not the pre-save state.
    expect(
        await waitForReference(client, headerUri, 2, 11, closedUri),
        "closed TU was not reindexed against the hook-rewritten disk",
    ).toBe(true);
    expect(await referenceUris(client, headerUri, 1, 11)).not.toContain(closedUri);
});
