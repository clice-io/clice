/// Tests for workspace helpers (tools/client/workspace.ts).

import { expect, test } from "vitest";
import { canonicalUri } from "@clice/tools/workspace";

test("canonical uri edges", () => {
    // The identity canonicalizer must be a faithful percent-decode:
    // collision-free for literal '%', inert on '+', UTF-8 aware, and an
    // identity on malformed input.
    expect(canonicalUri("file:///ws/a%20b.h")).toBe("file:///ws/a b.h");
    expect(canonicalUri("file:///ws/a%2520b.h")).toBe("file:///ws/a%20b.h");
    expect(canonicalUri("file:///ws/a%2520b.h")).not.toBe(canonicalUri("file:///ws/a%20b.h"));
    expect(canonicalUri("file:///ws/a+b.h")).toBe("file:///ws/a+b.h");
    expect(canonicalUri("file:///ws/%E6%97%A5.h")).toBe("file:///ws/日.h");
    expect(canonicalUri("file:///ws/%GG.h")).toBe("file:///ws/%GG.h");
    // The two drive-colon spellings — client-encoded and server-literal —
    // collapse to one identity.
    expect(canonicalUri("file:///c%3A/x.h")).toBe(canonicalUri("file:///c:/x.h"));
});
