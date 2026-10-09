/// An index run whose worker is killed from outside — the death names no
/// request — is lost, not blamed: a later round indexes the file again.

import { expect, serve } from "../../fixtures.ts";

serve("shapes/headers", { killOn: "tuRun src/registry.cpp" })(
    "crash during indexing",
    async ({ s }) => {
        await s.indexed();
        expect((await s.counts()).files["src/registry.cpp"]?.index).toBe(2);
        expect(s.show(await s.workspaceSymbols("registry_count"))).toBe(
            "registry_count src/registry.cpp: int registry_count() {",
        );
        expect(s.workspace.log("master.log")).toContain("Worker died while indexing");
    },
);
