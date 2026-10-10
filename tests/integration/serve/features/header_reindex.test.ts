/// Saving a header must reindex the closed TUs that include it.

import type { Serve } from "@clice/tools/actions";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

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

// alpha and beta keep their positions across versions, so lookups on the
// open header stay valid whichever text it holds.
const ALPHA = at("header.h", "alpha()");
const BETA = at("header.h", "beta()");

/// The files whose rows reference the symbol at `loc`.
async function referrers(s: Serve, loc: Loc): Promise<string[]> {
    return [...new Set(((await s.references(loc)) ?? []).map((site) => s.relative(site.uri)))];
}

const test = serve.files(
    { "header.h": HEADER_V1, "closed.cpp": CLOSED_TU },
    { manifest: { cxx: ["-std=c++17"], units: { "closed.cpp": [] } } },
);

test("header save reindexes dependents", async ({ s }) => {
    await s.compiled("header.h");

    // Initial background index: the closed TU's call resolves to alpha.
    await s.indexed();
    expect(
        await referrers(s, ALPHA),
        "initial index never produced the closed TU's alpha reference",
    ).toContain("closed.cpp");
    expect(await referrers(s, BETA)).not.toContain("closed.cpp");

    s.edit("header.h", { text: HEADER_V2 });
    s.save("header.h");

    // The closed TU is reindexed against the saved header: its call now
    // references beta, and the stale alpha reference is gone.
    await s.indexed();
    expect(await referrers(s, BETA), "closed TU was not reindexed after the header save").toContain(
        "closed.cpp",
    );
    expect(await referrers(s, ALPHA)).not.toContain("closed.cpp");
});

test("divergent save follows disk", async ({ s }) => {
    await s.compiled("header.h");

    await s.indexed();
    expect(
        await referrers(s, ALPHA),
        "initial index never produced the closed TU's alpha reference",
    ).toContain("closed.cpp");

    // A save hook rewrote the file as the save landed: the disk holds V2
    // while the buffer still holds V1 and no didChange is ever sent.
    s.disk.write("header.h", HEADER_V2);
    s.save("header.h", { write: false });

    // Dependents must follow the disk truth, not the pre-save state.
    await s.indexed();
    expect(
        await referrers(s, BETA),
        "closed TU was not reindexed against the hook-rewritten disk",
    ).toContain("closed.cpp");
    expect(await referrers(s, ALPHA)).not.toContain("closed.cpp");
});
