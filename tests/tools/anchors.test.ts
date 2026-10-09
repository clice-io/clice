/// The anchor check over the serve scenarios (tools/client/anchors.ts).

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { checkAnchors } from "@clice/tools/anchors";

const INTEGRATION = path.join(import.meta.dirname, "..", "integration");

test("every anchor stands once", () => {
    const files = fs
        .readdirSync(INTEGRATION, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".test.ts"))
        .map((name) => path.join(INTEGRATION, name));
    expect(checkAnchors(files)).toEqual([]);
});

test("a missing anchor is reported", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-anchors-"));
    const file = path.join(dir, "case.test.ts");
    fs.writeFileSync(
        file,
        [
            'const TABLE = [at("main.cpp", "add(1, 2)")];',
            'serve("tiny").for(TABLE)("x", async (item, { s }) => {',
            '    s.edit("main.cpp", { after: "return", insert: " " });',
            "});",
            'serve.each(["shapes/headers", "shapes/modules"])("y", async ({ s }) => {',
            '    at("app/main.cpp", "registry_|count()");',
            '    s.disk.edit(s.file("registry"), { replace: "#include", with: "" });',
            '    at("include/shapes/registry.h", "int registry_count();");',
            "});",
        ].join("\n"),
    );
    try {
        expect(checkAnchors([file])).toEqual([
            'case.test.ts:3: tiny: main.cpp has "return" 2 times',
            'case.test.ts:7: shapes/modules: src/registry.cpp has "#include" 0 times',
            "case.test.ts:8: shapes/modules: no file include/shapes/registry.h",
        ]);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
