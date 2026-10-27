import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Every indexing round shows its progress. A client refuses to create a
// token it still holds, so a round that ends while the client creates its
// token ends that token, or no round after it could show.

const PROGRESS = "clice/backgroundIndex";

test(async (t) => {
    const s = await t.start();
    await s.indexed();
    // The first round may have ended while its token was created, its
    // token ending after the server went idle.
    const created = s.client.progressTokens.filter((token) => token === PROGRESS).length;
    await s.client.progressed(PROGRESS, "end", created);
    const kinds = () =>
        s.client.progressEvents
            .filter((event) => event.token === PROGRESS)
            .map((event) => (event.value as { kind: string }).kind)
            .filter((kind) => kind !== "report");
    const shown = kinds().length;
    const count = (kind: string) => kinds().filter((each) => each === kind).length;

    // A round ending before the client answers for its token.
    const answers: (() => void)[] = [];
    s.client.progressCreateHold = new Promise((resolve) => answers.push(resolve));
    const ends = count("end");
    t.disk.edit("other.cpp", { replace: "return 2;", with: "return 3;" });
    await s.sync({ poll: true });
    answers.forEach((answer) => {
        answer();
    });
    s.client.progressCreateHold = undefined;
    await s.client.progressed(PROGRESS, "end", ends + 1);

    // The round after it shows, held until the client sees it begin.
    const begins = count("begin");
    const held = await s.park("index", "other.cpp", { at: "reply" });
    t.disk.edit("other.cpp", { replace: "return 3;", with: "return 4;" });
    expect(await s.poll("workspace")).toBe(1);
    await held.reached();
    await s.client.progressed(PROGRESS, "begin", begins + 1);
    await held.release();
    await s.indexed();
    expect(kinds().slice(shown)).toEqual(["begin", "end", "begin", "end"]);
});
