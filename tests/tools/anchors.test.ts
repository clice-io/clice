/// The anchor check over the serve scenarios (tools/client/anchors.ts).

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { checkAnchors } from "@clice/tools/anchors";
import { SAMPLES_DIR, readManifest } from "@clice/tools/project";

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
            '    s.disk.edit(s.file("registry"), { replace: "shapes/registry.h", with: "" });',
            '    at("include/shapes/registry.h", "int registry_count();");',
            "});",
            "const KINDS = { tiny: 1 };",
            'serve.each(Object.keys(KINDS))("z", async ({ s }) => {',
            '    s.edit("main.cpp", { remove: "gone" });',
            "});",
            'serve.each(["tiny", name])("w", async () => {});',
            'const loose = serve.files({ "main.cpp": "" }).skipIf(false);',
            'loose("v", async () => {',
            '    at("main.cpp", "|absent");',
            "});",
            'const bound = serve("tiny").skipIf(false);',
            'bound("u", async () => {',
            '    at("main.cpp", "|absent");',
            "});",
        ].join("\n"),
    );
    try {
        expect(checkAnchors([file])).toEqual([
            "case.test.ts:14: serve.each() names a project the check cannot read",
            'case.test.ts:3: tiny: main.cpp has "return" more than once',
            'case.test.ts:7: shapes/modules: src/registry.cpp has no "shapes/registry.h"',
            "case.test.ts:8: shapes/modules: no file include/shapes/registry.h",
            'case.test.ts:12: tiny: main.cpp has no "gone"',
            'case.test.ts:21: tiny: main.cpp has no "absent"',
        ]);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("variants name the same files", () => {
    const shapes = path.join(SAMPLES_DIR, "shapes");
    const variants = fs.readdirSync(shapes).map((variant) => `shapes/${variant}`);
    const names = variants.map((variant) => {
        const files = readManifest(variant).files ?? {};
        for (const [name, file] of Object.entries(files)) {
            expect(fs.existsSync(path.join(SAMPLES_DIR, variant, file)), `${variant} ${name}`).toBe(
                true,
            );
        }
        return Object.keys(files).sort();
    });
    for (const other of names.slice(1)) {
        expect(other).toEqual(names[0]);
    }
});
