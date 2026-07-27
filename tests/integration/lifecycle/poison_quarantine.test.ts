/// A poison document must not kill the session (F19 quarantine).
///
/// A file whose compiles crash the stateful worker is quarantined after two
/// consecutive crashes: healthy documents keep answering through it, the poison
/// file carries an explanatory diagnostic instead of an empty list, and an edit
/// that fixes it earns a probe that brings it back.

import type * as proto from "vscode-languageserver-protocol";
import { sleep } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

function poison(n: number): string {
    return `int add(int a, int b) { return a + b; }\n// edit ${n}\n#pragma clang __debug crash\n`;
}

// The pragma before any declaration lands in the preamble: the PCH build
// crashes the stateless worker before any stateful compile runs.
function poisonPreamble(n: number): string {
    return `#pragma clang __debug crash\n// edit ${n}\nint add(int a, int b) { return a + b; }\n`;
}

const FIXED = "int add(int a, int b) { return a + b; }\n";
const HEALTHY = "int healthy(int x) { return x * 2; }\n";

function isQuarantined(diags: proto.Diagnostic[]): boolean {
    return diags.some((d) => {
        const message = typeof d.message === "string" ? d.message : d.message.value;
        return message.includes("quarantined");
    });
}

test("poison quarantine", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("healthy.cpp", HEALTHY);
    workspace.write("poison.cpp", poison(0));
    workspace.writeCDB(["healthy.cpp", "poison.cpp"]);

    // Worker crashes are the point of this test, so the session opts out of
    // the anomaly gate; Debug builds trap anomalies with abort() unless told
    // otherwise (same as crash_recovery.test).
    process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
    let client;
    try {
        client = session.spawn(workspace, { allowAnomaly: true });
        await client.initialize(workspace);
    } finally {
        delete process.env["CLICE_ANOMALY_NO_TRAP"];
    }

    const [healthyUri] = client.open("healthy.cpp");
    const [poisonUri] = client.open("poison.cpp");

    const hover = await client.hoverAt(healthyUri, 0, 5);
    expect(hover, "healthy baseline hover failed").not.toBeNull();

    const healthyAnswers = async (context: string): Promise<void> => {
        // The first two crashes may transiently take healthy's worker
        // with them (sub-second respawn); the guarantee under test is
        // that healthy is never PERMANENTLY lost.
        for (let i = 0; i < 10; i++) {
            if ((await client.hoverAt(healthyUri, 0, 5)) !== null) {
                return;
            }
            await sleep(500);
        }
        throw new Error(`healthy hover permanently lost: ${context}`);
    };

    // Hammer the poison document: two crashes reach the quarantine
    // threshold, each edit past it earns only a single probe. Healthy
    // must keep answering through all of it.
    for (let i = 1; i < 5; i++) {
        await client.hoverAt(poisonUri, 0, 5);
        client.change(poisonUri, i, poison(i));
        await sleep(300);
        await healthyAnswers(`poison cycle ${i}`);
    }

    // The quarantined document explains itself.
    let deadline = 20;
    while (deadline > 0) {
        if (isQuarantined(client.diagnostics.get(poisonUri) ?? [])) {
            break;
        }
        await sleep(500);
        deadline -= 1;
    }
    expect(
        isQuarantined(client.diagnostics.get(poisonUri) ?? []),
        `missing quarantine diagnostic: ${JSON.stringify(client.diagnostics.get(poisonUri) ?? [])}`,
    ).toBe(true);

    // Fixing the file earns a probe that brings it back.
    client.change(poisonUri, 100, FIXED);
    let recovered = null;
    for (let i = 0; i < 20; i++) {
        await sleep(500);
        recovered = await client.hoverAt(poisonUri, 0, 5);
        if (recovered !== null) {
            break;
        }
    }
    expect(recovered, "fixed poison file did not recover").not.toBeNull();

    const afterHover = await client.hoverAt(healthyUri, 0, 5);
    expect(afterHover, "healthy hover failed after recovery").not.toBeNull();
});

test("poison preamble quarantine", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("healthy.cpp", HEALTHY);
    workspace.write("poison.cpp", poisonPreamble(0));
    workspace.writeCDB(["healthy.cpp", "poison.cpp"]);

    process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
    let client;
    try {
        client = session.spawn(workspace, { allowAnomaly: true });
        await client.initialize(workspace);
    } finally {
        delete process.env["CLICE_ANOMALY_NO_TRAP"];
    }

    const [healthyUri] = client.open("healthy.cpp");
    const [poisonUri] = client.open("poison.cpp");

    // PCH-build crashes count toward the same streak as compile crashes,
    // so a poison preamble must still reach quarantine.
    for (let i = 1; i < 4; i++) {
        await client.hoverAt(poisonUri, 2, 5);
        client.change(poisonUri, i, poisonPreamble(i));
        await sleep(300);
    }

    let deadline = 20;
    while (deadline > 0) {
        if (isQuarantined(client.diagnostics.get(poisonUri) ?? [])) {
            break;
        }
        await sleep(500);
        deadline -= 1;
    }
    expect(
        isQuarantined(client.diagnostics.get(poisonUri) ?? []),
        `missing quarantine diagnostic: ${JSON.stringify(client.diagnostics.get(poisonUri) ?? [])}`,
    ).toBe(true);

    // Healthy documents survive the stateless carnage.
    let hover = null;
    for (let i = 0; i < 10; i++) {
        hover = await client.hoverAt(healthyUri, 0, 5);
        if (hover !== null) {
            break;
        }
        await sleep(500);
    }
    expect(hover, "healthy hover lost to poison preamble").not.toBeNull();

    // Fixing the preamble earns a probe that brings the file back.
    client.change(poisonUri, 100, FIXED);
    let recovered = null;
    for (let i = 0; i < 20; i++) {
        await sleep(500);
        recovered = await client.hoverAt(poisonUri, 0, 5);
        if (recovered !== null) {
            break;
        }
    }
    expect(recovered, "fixed preamble did not recover").not.toBeNull();
});
