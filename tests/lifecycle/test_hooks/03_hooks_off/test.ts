import { WatchRequest } from "@clice/tools/protocol";
import { refusal, test } from "@clice/tools/testing";

// The test hooks answer only a server started with CLICE_TEST_HOOKS=1:
// an editor's server refuses them and counts no builds.

test(async (t) => {
    const s = await t.start({ env: { CLICE_TEST_HOOKS: "0" } });
    const uri = s.uri("main.cpp");
    await t.show("refused", {
        sync: await refusal(s.client.sync()),
        hold: await refusal(s.client.hold("compile", uri)),
        gate: await refusal(s.client.gate("compile", uri)),
        release: await refusal(s.client.release(1)),
        watch: await refusal(s.client.sendRequest(WatchRequest, { event: "checkpoint" })),
    });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("builds counted", (await s.stats()).builds.length);
});
