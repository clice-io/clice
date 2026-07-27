/// Saving a header must reindex the closed TUs that include it.

import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

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
    tmp.write("header.h", HEADER_V1);
    tmp.write("closed.cpp", CLOSED_TU);
    tmp.writeCDB(["closed.cpp"]);
    const client = session.spawn(tmp);
    await client.initialize(tmp);

    const headerUri = tmp.uri("header.h");
    const closedUri = tmp.uri("closed.cpp");
    await client.openAndWait("header.h");

    // Initial background index: the closed TU's call resolves to alpha.
    expect(
        await client.waitForReference(headerUri, 1, 11, closedUri),
        "initial index never produced the closed TU's alpha reference",
    ).toBe(true);
    expect(await client.referenceUris(headerUri, 2, 11)).not.toContain(closedUri);

    await sleep(MTIME_GRANULARITY);
    tmp.write("header.h", HEADER_V2);
    client.change(headerUri, 2, HEADER_V2);
    client.save(headerUri);

    // The closed TU is reindexed against the saved header: its call now
    // references beta, and the stale alpha reference is gone.
    expect(
        await client.waitForReference(headerUri, 2, 11, closedUri),
        "closed TU was not reindexed after the header save",
    ).toBe(true);
    expect(await client.referenceUris(headerUri, 1, 11)).not.toContain(closedUri);
});

test("divergent save follows disk", async ({ session }) => {
    const tmp = session.tmpdir();
    tmp.write("header.h", HEADER_V1);
    tmp.write("closed.cpp", CLOSED_TU);
    tmp.writeCDB(["closed.cpp"]);
    const client = session.spawn(tmp);
    await client.initialize(tmp);

    const headerUri = tmp.uri("header.h");
    const closedUri = tmp.uri("closed.cpp");
    await client.openAndWait("header.h");

    expect(
        await client.waitForReference(headerUri, 1, 11, closedUri),
        "initial index never produced the closed TU's alpha reference",
    ).toBe(true);

    // A save hook rewrote the file as the save landed: the disk holds V2
    // while the buffer still holds V1 and no didChange is ever sent.
    // (alpha/beta keep their positions across versions, so buffer-resolved
    // lookups on the open header stay valid.)
    await sleep(MTIME_GRANULARITY);
    tmp.write("header.h", HEADER_V2);
    client.save(headerUri);

    // Dependents must follow the disk truth, not the pre-save state.
    expect(
        await client.waitForReference(headerUri, 2, 11, closedUri),
        "closed TU was not reindexed against the hook-rewritten disk",
    ).toBe(true);
    expect(await client.referenceUris(headerUri, 1, 11)).not.toContain(closedUri);
});
