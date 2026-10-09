/// An index run whose worker is killed from outside — the death names no
/// request — is lost, not blamed: a later round indexes the file again.

import { expect, serve } from "../../fixtures.ts";

serve("shapes/headers", { killOn: { request: "tuRun", file: "src/registry.cpp" } })(
    "crash during indexing",
    async ({ s }) => {
        await s.indexed();
        const counts = await s.counts();
        for (const unit of Object.keys(s.manifest.units)) {
            expect(counts.files[unit]?.index, unit).toBe(unit === "src/registry.cpp" ? 2 : 1);
        }
        expect(s.show(await s.workspaceSymbols("registry_count"))).toBe(
            "registry_count src/registry.cpp: int registry_count() {",
        );
        expect(s.workspace.log("master.log")).toContain("Worker died while indexing");
    },
);
