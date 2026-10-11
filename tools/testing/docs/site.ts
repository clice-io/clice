/// Fill the generated regions of the pages given, keyed by
/// `<topic>/<subtopic>`, with that section's cards; the cards' data comes
/// from the cases run under CLICE_CARDS_DIR.
///
/// Usage: node tools/testing/docs/site.ts page.md...

import * as fs from "node:fs";
import { rewriteRegions } from "../../docs/generated.ts";
import { loadCases } from "../cases/case.ts";
import { CASES_DIR } from "../cases/project.ts";
import { regionItems } from "./pages.ts";

const SECTION = {
    begin: /^<!-- BEGIN GENERATED ITEMS: ([a-z0-9_]+\/[a-z0-9_]+) -->$/,
    end: "<!-- END GENERATED ITEMS -->",
};

const pages = process.argv.slice(2);
if (pages.length === 0) {
    console.error("Usage: node tools/testing/docs/site.ts page.md...");
    process.exit(64);
}
const cases = loadCases(CASES_DIR);
const problems: string[] = [];
for (const page of pages) {
    const { text } = rewriteRegions(
        fs.readFileSync(page, "utf8"),
        page,
        SECTION,
        (key) => {
            const [topic = "", subtopic = ""] = key.split("/");
            return regionItems(cases, topic, subtopic);
        },
        problems,
    );
    fs.writeFileSync(page, text);
}
if (problems.length > 0) {
    console.error(problems.join("\n"));
    process.exit(1);
}
