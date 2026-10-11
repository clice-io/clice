/// Write the docs the documented cases make, as standalone markdown.
///
/// Usage: node tools/testing/docs/preview.ts <dir>

import * as fs from "node:fs";
import * as path from "node:path";
import { loadCases } from "../cases/case.ts";
import { CASES_DIR } from "../cases/project.ts";
import { previewPages } from "./pages.ts";

const [dir] = process.argv.slice(2);
if (dir === undefined) {
    console.error("Usage: node tools/testing/docs/preview.ts <dir>");
    process.exit(64);
}
for (const [page, text] of previewPages(loadCases(CASES_DIR))) {
    const target = path.join(dir, page);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, text);
    console.log(target);
}
