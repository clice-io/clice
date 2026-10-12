/// The documentation the cases make, against what the repository holds: the
/// generated regions of the pages under docs/en, keyed `<topic>/<subtopic>`,
/// each listing that section's documented cases; the features overview,
/// each feature page's cases counted by status; and docs/cards, one card
/// per documented case, which the cases run writes. `update` rewrites the
/// regions and deletes the cards no case owns; `check` fails where pages,
/// cases and cards disagree.
///
/// Usage: node tools/testing/docs/site.ts check|update

import * as fs from "node:fs";
import * as path from "node:path";
import yaml from "js-yaml";
import { renderMarkdownTable, rewriteRegions } from "../../docs/generated.ts";
import { loadCases } from "../cases/case.ts";
import { REPO_ROOT } from "../../repo.ts";
import { CASES_DIR } from "../cases/project.ts";
import { CARDS_DIR, cardPath } from "./cards.ts";
import { regionItems, status } from "./pages.ts";

const SECTION = {
    begin: /^<!-- BEGIN GENERATED ITEMS: ([a-z0-9_]+\/[a-z0-9_]+) -->$/,
    end: "<!-- END GENERATED ITEMS -->",
};

const OVERVIEW = {
    begin: /^<!-- BEGIN GENERATED OVERVIEW -->$/,
    end: "<!-- END GENERATED OVERVIEW -->",
};

const PAGES_DIR = path.join(REPO_ROOT, "docs", "en");
const FEATURES_DIR = path.join(PAGES_DIR, "features");
const OVERVIEW_PAGE = path.join(FEATURES_DIR, "overview.md");

const mode = process.argv[2];
if (mode !== "check" && mode !== "update") {
    console.error("Usage: node tools/testing/docs/site.ts check|update");
    process.exit(64);
}

function files(dir: string, suffix: string): string[] {
    return fs
        .readdirSync(dir, { recursive: true, encoding: "utf8" })
        .filter((rel) => rel.endsWith(suffix))
        .map((rel) => path.join(dir, rel))
        .sort();
}

const cases = loadCases(CASES_DIR);
const documented = cases.filter((c) => c.readme !== undefined);
const problems: string[] = [];
const shown = new Map<string, string>();
/// The sections each page lists, by page.
const sections = new Map<string, string[]>();

function update(page: string, text: string, next: string): void {
    if (next === text) {
        return;
    }
    if (mode === "update") {
        fs.writeFileSync(page, next);
    } else {
        problems.push(`${path.relative(REPO_ROOT, page)}: its generated regions are stale`);
    }
}

for (const page of files(PAGES_DIR, ".md")) {
    const rel = path.relative(REPO_ROOT, page);
    const text = fs.readFileSync(page, "utf8");
    const { text: next, seen } = rewriteRegions(
        text,
        rel,
        SECTION,
        (key) => {
            const [topic = "", subtopic = ""] = key.split("/");
            return regionItems(cases, topic, subtopic);
        },
        problems,
    );
    sections.set(page, [...seen]);
    for (const key of seen) {
        const other = shown.get(key);
        if (other !== undefined) {
            problems.push(`${rel}: '${key}' is listed on ${other} already`);
        }
        shown.set(key, rel);
        if (!documented.some((c) => `${c.topic}/${c.subtopic}` === key)) {
            problems.push(`${rel}: '${key}' has no documented case`);
        }
    }
    update(page, text, next);
}

/// Each feature page in the sidebar's order, its title and the statuses of
/// the cases it shows.
function overview(): string {
    const sidebar = yaml.load(fs.readFileSync(path.join(PAGES_DIR, "sidebar.yaml"), "utf8")) as {
        features: { items: string[] };
    };
    const rows = sidebar.features.items
        .filter((name) => name !== "overview")
        .map((name) => {
            const page = path.join(FEATURES_DIR, `${name}.md`);
            const title = /^# (.+)$/m.exec(fs.readFileSync(page, "utf8"))?.[1] ?? name;
            const keys = new Set(sections.get(page) ?? []);
            const counts = new Map<string, number>();
            for (const c of documented) {
                if (keys.has(`${c.topic}/${c.subtopic}`)) {
                    counts.set(status(c), (counts.get(status(c)) ?? 0) + 1);
                }
            }
            const statuses = ["supported", "partial", "unsupported"]
                .filter((name) => counts.has(name))
                .map((name) => `${String(counts.get(name))} ${name}`);
            return [`[${title}](./${name}.md)`, statuses.join(" · ")];
        });
    return renderMarkdownTable([["Feature", "Status"], ...rows]).join("\n");
}

{
    const text = fs.readFileSync(OVERVIEW_PAGE, "utf8");
    const { text: next } = rewriteRegions(
        text,
        path.relative(REPO_ROOT, OVERVIEW_PAGE),
        OVERVIEW,
        overview,
        problems,
    );
    update(OVERVIEW_PAGE, text, next);
}

for (const key of new Set(documented.map((c) => `${c.topic}/${c.subtopic}`))) {
    if (!shown.has(key)) {
        problems.push(`documented cases of '${key}' are on no page: give it a region`);
    }
}

const owned = new Set(documented.map((c) => cardPath(c.id)));
for (const c of documented) {
    if (!fs.existsSync(cardPath(c.id))) {
        problems.push(`${c.id}: no card; the cases run writes it under UPDATE_SNAPSHOTS=1`);
    }
}
if (fs.existsSync(CARDS_DIR)) {
    for (const card of files(CARDS_DIR, ".json")) {
        if (owned.has(card)) {
            continue;
        }
        if (mode === "update") {
            fs.rmSync(card);
        } else {
            problems.push(`${path.relative(REPO_ROOT, card)}: no documented case owns it`);
        }
    }
}

if (problems.length > 0) {
    console.error(problems.join("\n"));
    if (mode === "check") {
        console.error("pixi run update-feature-docs rewrites the regions and drops orphan cards");
    }
    process.exit(1);
}
