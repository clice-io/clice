import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// The test hooks answer only a server started with project.test_hooks:
// an editor's server refuses them and counts no builds.

async function refusal(reply: Promise<unknown>): Promise<{ code: number; message: string }> {
    const error = await reply.then(
        () => null,
        (failure: unknown) => failure,
    );
    if (!(error instanceof proto.ResponseError)) {
        throw new Error(`expected an error reply, got ${JSON.stringify(error)}`);
    }
    return { code: error.code, message: error.message };
}

test(async (t) => {
    const s = await t.start({ config: { project: { test_hooks: false } } });
    const uri = s.uri("main.cpp");
    await t.show("refused", {
        sync: await refusal(s.client.sync()),
        hold: await refusal(s.client.hold("compile", uri)),
        gate: await refusal(s.client.gate("compile", uri)),
        release: await refusal(s.client.release(1)),
    });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("builds counted", (await s.stats()).builds.length);
});
