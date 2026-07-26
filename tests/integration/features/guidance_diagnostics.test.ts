/// Guidance diagnostics for files compiled with guessed compile commands.
///
/// When a file has no compilation database entry, clice compiles it with a
/// synthesized fallback command. If that produces file-not-found errors, a
/// file-top warning explains the situation; an exact CDB match never gets it.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { assertNoAnomaly, guidanceMessages } from "../../tools/checks.ts";
import type { CliceClient } from "../../tools/client.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";

const GUIDANCE_CODE = "inferred-compile-command";

const BROKEN_INCLUDE = '#include "no_such_header.h"\nint main() { return 0; }\n';

function mkTmp(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
}

function guidanceDiags(client: CliceClient, uri: string): proto.Diagnostic[] {
    return (client.diagnostics.get(uri) ?? []).filter((d) => d.code === GUIDANCE_CODE);
}

function fileNotFoundDiags(client: CliceClient, uri: string): proto.Diagnostic[] {
    return (client.diagnostics.get(uri) ?? []).filter((d) => d.code === "err_pp_file_not_found");
}

test("fallback guidance lifecycle", async () => {
    const tmp = mkTmp();
    try {
        fs.writeFileSync(path.join(tmp, "main.cpp"), BROKEN_INCLUDE);

        // Phase 1: no CDB — fallback command, broken include → guidance at the top.
        let client = await makeClient(cliceExecutable(), tmp);
        try {
            const [uri] = await client.openAndWait(path.join(tmp, "main.cpp"));
            expect(
                fileNotFoundDiags(client, uri).length,
                "broken include should surface",
            ).toBeGreaterThan(0);
            const guidance = guidanceDiags(client, uri);
            expect(
                guidance.length,
                `expected one guidance diagnostic: ${JSON.stringify(guidance)}`,
            ).toBe(1);
            expect(guidance[0]!.severity).toBe(proto.DiagnosticSeverity.Warning);
            expect(guidance[0]!.range.start.line).toBe(0);
            expect(guidance[0]!.source).toBe("clice");
            // The missing CDB is also announced via window/logMessage guidance.
            expect(guidanceMessages(client).some((m) => m.includes("compile_commands.json"))).toBe(
                true,
            );
            assertNoAnomaly(client, tmp);
        } finally {
            await shutdownClient(client);
        }

        // Phase 2: provide a CDB and restart — the include error remains, the
        // guidance diagnostic must disappear (exact CDB match never gets it).
        writeCdb(tmp, ["main.cpp"]);
        client = await makeClient(cliceExecutable(), tmp);
        try {
            const [uri] = await client.openAndWait(path.join(tmp, "main.cpp"));
            expect(
                fileNotFoundDiags(client, uri).length,
                "include is still broken",
            ).toBeGreaterThan(0);
            expect(
                guidanceDiags(client, uri).length,
                "CDB-matched files must not get the inferred-command guidance",
            ).toBe(0);
            assertNoAnomaly(client, tmp);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("fallback applies rule appends", async () => {
    const tmp = mkTmp();
    try {
        // Without a CDB, include paths supplied via clice.toml rules must reach
        // the synthesized fallback command.
        fs.mkdirSync(path.join(tmp, "inc"));
        fs.writeFileSync(path.join(tmp, "inc", "dep.h"), "#pragma once\nconstexpr int dep = 1;\n");
        fs.writeFileSync(
            path.join(tmp, "main.cpp"),
            '#include "dep.h"\nint main() { return dep; }\n',
        );
        const includeDir = path.join(tmp, "inc").split(path.sep).join("/");
        fs.writeFileSync(
            path.join(tmp, "clice.toml"),
            `[[rules]]\npatterns = ["**/*.cpp"]\nappend = ["-I${includeDir}"]\n`,
        );

        const client = await makeClient(cliceExecutable(), tmp);
        try {
            const [uri] = await client.openAndWait(path.join(tmp, "main.cpp"));
            expect(
                fileNotFoundDiags(client, uri).length,
                "rule -I must reach the fallback command",
            ).toBe(0);
            expect(guidanceDiags(client, uri).length).toBe(0);
            assertNoAnomaly(client, tmp);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("fallback clean no guidance", async () => {
    const tmp = mkTmp();
    try {
        // A guessed command that works produces no guidance noise.
        fs.writeFileSync(path.join(tmp, "main.cpp"), "int main() { return 0; }\n");
        const client = await makeClient(cliceExecutable(), tmp);
        try {
            const [uri] = await client.openAndWait(path.join(tmp, "main.cpp"));
            expect(guidanceDiags(client, uri).length).toBe(0);
            assertNoAnomaly(client, tmp);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});
