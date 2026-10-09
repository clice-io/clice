/// Guidance diagnostics for files compiled with guessed compile commands.
///
/// When a file has no compilation database entry, clice compiles it with a
/// synthesized fallback command. If that produces file-not-found errors, a
/// file-top warning explains the situation; an exact CDB match never gets it.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { writeDatabase } from "@clice/tools/project";
import { expect, serve } from "../../fixtures.ts";

const GUIDANCE_CODE = "inferred-compile-command";

const BROKEN_INCLUDE = '#include "no_such_header.h"\nint main() { return 0; }\n';

function guidanceDiags(diagnostics: proto.Diagnostic[]): proto.Diagnostic[] {
    return diagnostics.filter((d) => d.code === GUIDANCE_CODE);
}

function fileNotFoundDiags(diagnostics: proto.Diagnostic[]): proto.Diagnostic[] {
    return diagnostics.filter((d) => d.code === "err_pp_file_not_found");
}

/// A server started on the workspace without a compile_commands.json,
/// which a case's workspace always has: it is removed while no server
/// runs, with `body`'s changes.
function withoutDatabase(s: Serve, body: () => void = () => undefined): Promise<void> {
    return s.offline(() => {
        s.workspace.rm("compile_commands.json");
        body();
    });
}

const NO_UNITS = { manifest: { units: {} } };

serve.files({ "main.cpp": BROKEN_INCLUDE }, NO_UNITS)(
    "fallback guidance lifecycle",
    async ({ s }) => {
        // Phase 1: no CDB — fallback command, broken include → guidance at the top.
        await withoutDatabase(s);
        const first = await s.compiled("main.cpp");
        const missingIncludes = fileNotFoundDiags(first);
        expect(missingIncludes.length, "broken include should surface").toBeGreaterThan(0);
        expect(missingIncludes.every((diagnostic) => diagnostic.source === "clang")).toBe(true);
        const guidance = guidanceDiags(first);
        expect(
            guidance.length,
            `expected one guidance diagnostic: ${JSON.stringify(guidance)}`,
        ).toBe(1);
        expect(guidance[0]!.severity).toBe(proto.DiagnosticSeverity.Warning);
        expect(guidance[0]!.range.start.line).toBe(0);
        expect(guidance[0]!.source).toBe("clice");
        expect(guidance[0]!.codeDescription?.href).toBe(
            "https://docs.clice.io/clice/guide/quick-start#project-setup",
        );
        // The missing CDB is also announced via window/logMessage guidance.
        expect(s.client.guidanceMessages().some((m) => m.includes("compile_commands.json"))).toBe(
            true,
        );

        // Phase 2: provide a CDB and restart — the include error remains, the
        // guidance diagnostic must disappear (exact CDB match never gets it).
        await s.offline(() => {
            writeDatabase(s.workspace, { cxx: ["-std=c++17"], units: { "main.cpp": [] } });
        });
        const second = await s.compiled("main.cpp");
        expect(fileNotFoundDiags(second).length, "include is still broken").toBeGreaterThan(0);
        expect(
            guidanceDiags(second).length,
            "CDB-matched files must not get the inferred-command guidance",
        ).toBe(0);
    },
);

// Without a CDB, include paths supplied via clice.toml rules must reach the
// synthesized fallback command.
serve.files(
    {
        "inc/dep.h": "#pragma once\nconstexpr int dep = 1;\n",
        "main.cpp": '#include "dep.h"\nint main() { return dep; }\n',
    },
    NO_UNITS,
)("fallback applies rule appends", async ({ s }) => {
    const includeDir = s.workspace.path("inc").replaceAll("\\", "/");
    await withoutDatabase(s, () => {
        s.disk.write(
            "clice.toml",
            `[[rules]]\npatterns = ["**/*.cpp"]\nappend = ["-I${includeDir}"]\n`,
        );
    });
    const diagnostics = await s.compiled("main.cpp");
    expect(fileNotFoundDiags(diagnostics).length, "rule -I must reach the fallback command").toBe(
        0,
    );
    expect(guidanceDiags(diagnostics).length).toBe(0);
});

// A guessed command that works produces no guidance noise.
serve.files({ "main.cpp": "int main() { return 0; }\n" }, NO_UNITS)(
    "fallback clean no guidance",
    async ({ s }) => {
        await withoutDatabase(s);
        expect(guidanceDiags(await s.compiled("main.cpp")).length).toBe(0);
    },
);
