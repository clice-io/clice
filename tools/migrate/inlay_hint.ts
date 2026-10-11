/// One-off check of the inlay_hint migration: every hint an old snapshot of
/// tests/snap/inlay_hint pinned is in the case it moved to, drawn into its
/// line as the new output draws it.
///
///     node tools/migrate/inlay_hint.ts
///
/// An old hint is its place and label, its kind and padding, and the
/// declarations its label links to. Where the new case shows the links,
/// they must be the old ones; elsewhere each old link must reach a
/// declaration spelling the name it links (the link cases pin where), and
/// the kind and padding must follow the one rule the categories case pins:
/// a label ending in `:` is a parameter hint padded on the right, one
/// opening `, ` lists default arguments, anything else is a type hint.

import * as fs from "node:fs";
import * as path from "node:path";
import yaml from "js-yaml";
import { snapCorpora, type SnapCorpus, type SnapFixture } from "../snap/corpus.ts";
import { parseSnap } from "../snap/snapshot.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { SourceText } from "../testing/render/position.ts";
import { readToml } from "../testing/render/value.ts";

const TOPIC = path.join(CASES_DIR, "inlay_hints");

/// Where each old fixture went, and the new test each old one is when it
/// changed name.
const MOVED: Record<string, { case: string; tests?: Record<string, string> }> = {
    "designator_hints/01_designator_basic.cpp": { case: "designator_hints/01_fields_and_indices" },
    "designator_hints/02_designator_nested.cpp": { case: "designator_hints/02_nested_aggregates" },
    "designator_hints/03_designator_anonymous.cpp": {
        case: "designator_hints/03_anonymous_members",
    },
    "designator_hints/04_designator_suppression.cpp": { case: "designator_hints/04_suppression" },
    "designator_hints/05_designator_aggregates_only.cpp": {
        case: "designator_hints/05_aggregates_only",
    },
    "designator_hints/06_designator_recovery.cpp": {
        case: "designator_hints/06_broken_initializers",
    },
    "designator_hints/07_designator_parenthesized.cpp": {
        case: "designator_hints/07_parenthesized_init",
    },
    "designator_hints/08_designator_library_array.cpp": {
        case: "designator_hints/08_library_arrays",
    },
    "forwarding_dependent_calls.cpp": { case: "parameter_hints/21_generic_lambda_forwarding" },
    "other_hint_kinds/01_template_parameter_hints.cpp": {
        case: "other_kinds/01_template_arguments",
    },
    "other_hint_kinds/02_ctad_arguments.cpp": { case: "other_kinds/02_ctad_arguments" },
    "other_hint_kinds/03_conversion_hints.cpp": { case: "other_kinds/03_implicit_conversions" },
    "parameter_hints/01_param_names.cpp": { case: "parameter_hints/02_constructor_calls" },
    "parameter_hints/02_param_suppression.cpp": { case: "parameter_hints/03_suppression" },
    "parameter_hints/03_param_setters_builtins.cpp": {
        case: "parameter_hints/04_setters_and_builtins",
    },
    "parameter_hints/04_param_references.cpp": { case: "parameter_hints/05_reference_markers" },
    "parameter_hints/05_param_forwarding.cpp": { case: "parameter_hints/06_forwarding" },
    "parameter_hints/06_param_definition_names.cpp": {
        case: "parameter_hints/07_definition_names",
    },
    "parameter_hints/07_param_function_objects.cpp": {
        case: "parameter_hints/08_function_objects",
    },
    "parameter_hints/08_param_deducing_this.cpp": { case: "parameter_hints/09_deducing_this" },
    "parameter_hints/09_param_dependent.cpp": { case: "parameter_hints/10_dependent_calls" },
    "parameter_hints/10_param_packs.cpp": { case: "parameter_hints/11_pack_expansions" },
    "parameter_hints/11_param_macros.cpp": { case: "parameter_hints/12_macros" },
    "parameter_hints/12_param_implicit_conversions.cpp": {
        case: "parameter_hints/13_implicit_conversions",
    },
    "parameter_hints/13_param_pseudo_objects.cpp": { case: "parameter_hints/14_pseudo_objects" },
    "parameter_hints/14_param_explicit_instantiation.cpp": {
        case: "parameter_hints/15_explicit_instantiation",
    },
    "parameter_hints/15_param_case_insensitive.cpp": { case: "parameter_hints/16_near_matches" },
    "parameter_hints/16_param_inherited_constructors.cpp": {
        case: "parameter_hints/17_inherited_constructors",
    },
    "parameter_hints/17_param_anonymous.cpp": { case: "parameter_hints/18_anonymous_parameters" },
    "parameter_hints/18_param_operators.cpp": { case: "parameter_hints/19_operators" },
    "parameter_hints/19_param_pack_constructors.cpp": {
        case: "parameter_hints/20_pack_constructors",
    },
    "parameter_hints/20_param_default_arguments.cpp": {
        case: "default_arguments/01_omitted_arguments",
    },
    "type_hints/01_type_auto.cpp": { case: "type_hints/01_auto_variables" },
    "type_hints/02_type_sugar.cpp": {
        case: "type_hints/02_type_sugar",
        tests: { answers: "default" },
    },
    "type_hints/03_type_structured_bindings.cpp": { case: "type_hints/03_structured_bindings" },
    "type_hints/04_type_lambdas.cpp": { case: "type_hints/04_lambdas" },
    "type_hints/05_type_auto_return.cpp": { case: "type_hints/05_deduced_return_types" },
    "type_hints/06_type_decltype.cpp": { case: "type_hints/06_decltype" },
    "type_hints/07_type_auto_params.cpp": { case: "type_hints/07_auto_parameters" },
    "type_hints/08_type_explicit_source.cpp": { case: "type_hints/08_explicit_initializers" },
    "type_hints/09_type_dependent.cpp": { case: "type_hints/09_dependent_auto" },
    "type_hints/10_type_scopes.cpp": { case: "type_hints/10_scopes" },
    "type_hints/11_type_bindings_tuple.cpp": { case: "type_hints/11_tuple_bindings" },
    "type_hints/12_type_conflicting_instantiations.cpp": {
        case: "type_hints/12_instantiated_templates",
    },
    "type_hints/13_type_links/main.cpp": { case: "links/02_type_links" },
    "type_link_scopes.cpp": { case: "links/01_type_link_scopes" },
};

/// Old fixtures that ran nowhere, an unsupported status keeping no snapshot:
/// their cases pin the empty answer as known wrong.
const UNPINNED = new Set([
    "designator_hints/07_designator_parenthesized.cpp",
    "other_hint_kinds/01_template_parameter_hints.cpp",
    "other_hint_kinds/02_ctad_arguments.cpp",
    "other_hint_kinds/03_conversion_hints.cpp",
]);

/// Tests that start servers themselves: each block one request.
const SCENARIOS = new Set(["links/05_client_labels", "edits/01_buffer_edits"]);

interface OldHint {
    pos: string;
    kind: string;
    label: string;
    links?: string[];
    padding_left?: boolean;
    padding_right?: boolean;
}

interface NewLine {
    line: string;
    hints?: {
        label: string;
        kind?: string;
        padding?: string[];
        links?: { value: string; target: string }[];
    }[];
}

function corpusOf(feature: string): SnapCorpus {
    const found = snapCorpora().find((c) => c.feature === feature);
    if (found === undefined) {
        throw new Error(`no ${feature} corpus`);
    }
    return found;
}

const corpus = corpusOf("inlay_hint");

function oldHints(fixture: SnapFixture): Map<string, OldHint[]> {
    const file = path.join(corpus.corpus, fixture.rel.replace(/\.cpp$/, ".snap.yml"));
    const snap = parseSnap(fs.readFileSync(file, "utf8"));
    if (snap === null) {
        throw new Error(`${file}: no front matter`);
    }
    const body = yaml.load(snap.body) ?? [];
    if (fixture.meta.config === undefined) {
        return new Map([["answers", body as OldHint[]]]);
    }
    return new Map(Object.entries(body as Record<string, OldHint[]>));
}

function newOutput(id: string): Record<string, Record<string, unknown>> {
    const file = path.join(TOPIC, id, "snap.toml");
    return readToml(fs.readFileSync(file, "utf8")) as Record<string, Record<string, unknown>>;
}

function point(text: string): { line: number; character: number } {
    const [line, character] = text.split(":").map(Number);
    return { line: line ?? 0, character: character ?? 0 };
}

/// An old link, `name ${WS}/<fixture dir>/file:L:C-L:C`, rendered as the
/// new output renders a link target from `query`.
function oldLink(
    link: string,
    fixture: SnapFixture,
    query: string,
): { value: string; target: string; spelled: string } {
    const match = /^(\S+) \$\{WS\}\/(.+):(\d+:\d+)-(\d+:\d+)$/.exec(link);
    if (match === null) {
        throw new Error(`${fixture.rel}: link ${link}`);
    }
    const [, value = "", file = "", from = "", to = ""] = match;
    const source = fixture.files.find((each) => each.rel === file);
    if (source === undefined) {
        throw new Error(`${fixture.rel}: link into ${file}`);
    }
    const text = new SourceText(file, source.source.content);
    const start = point(from);
    const end = point(to);
    const line = text.line(start.line);
    const marked =
        `${line.slice(0, start.character)}⟦${line.slice(start.character, end.character)}⟧${line.slice(end.character)}`.trim();
    const name = path.posix.basename(file);
    return {
        value,
        target: name === query ? marked : `${name} │ ${marked}`,
        spelled: line.slice(start.character, end.character),
    };
}

/// The old hints drawn into their lines, in the order of the lines.
function drawn(text: SourceText, hints: readonly OldHint[]): Map<number, string> {
    const byLine = new Map<number, OldHint[]>();
    for (const hint of hints) {
        const { line } = point(hint.pos);
        byLine.set(line, [...(byLine.get(line) ?? []), hint]);
    }
    const out = new Map<number, string>();
    for (const [line, list] of [...byLine].sort(([a], [b]) => a - b)) {
        const raw = text.line(line);
        let rendered = "";
        let at = 0;
        for (const hint of [...list].sort(
            (a, b) => point(a.pos).character - point(b.pos).character,
        )) {
            const { character } = point(hint.pos);
            rendered += `${raw.slice(at, character)}⟨${hint.label}⟩`;
            at = character;
        }
        out.set(line, (rendered + raw.slice(at)).trim());
    }
    return out;
}

const problems: string[] = [];
const added: string[] = [];
let oldAsked = 0;
let oldCount = 0;
let linksCompared = 0;
let linksSpelled = 0;

for (const fixture of corpus.fixtures) {
    const target = MOVED[fixture.rel];
    if (target === undefined) {
        problems.push(`${fixture.rel}: no target`);
        continue;
    }
    if (UNPINNED.has(fixture.rel)) {
        continue;
    }
    const entry = fixture.files.find((file) => file.rel === fixture.rel);
    if (entry === undefined) {
        throw new Error(`${fixture.rel}: no entry`);
    }
    const query = path.posix.basename(fixture.rel);
    const text = new SourceText(query, entry.source.content);
    const output = newOutput(target.case);
    for (const [test, hints] of oldHints(fixture)) {
        oldAsked += 2;
        const newTest = target.tests?.[test] ?? test;
        const where = `${fixture.rel} ${test} → ${target.case} ${newTest}`;
        const blocks = output[newTest];
        if (blocks === undefined) {
            problems.push(`${where}: no such test`);
            continue;
        }
        const lines: NewLine[] = Object.values(blocks).flatMap(
            (block) => (block as { inlay_hints?: NewLine[] }).inlay_hints ?? [],
        );
        const now = new Map(lines.map((line) => [line.line, line]));
        const before = drawn(text, hints);
        for (const [, line] of before) {
            if (!now.has(line)) {
                problems.push(`${where}: line ${line} is gone`);
            }
        }
        for (const line of now.keys()) {
            if (![...before.values()].includes(line)) {
                added.push(`${where}: ${line}`);
            }
        }
        for (const hint of hints) {
            oldCount += 1;
            const at = `${where} ${hint.pos} ${hint.label}`;
            const line = before.get(point(hint.pos).line) ?? "";
            const details = now.get(line)?.hints?.find((each) => each.label === hint.label);
            const links = (hint.links ?? []).map((link) => oldLink(link, fixture, query));
            if (details?.links !== undefined) {
                const expected = JSON.stringify(
                    links.map(({ value, target }) => ({ value, target })),
                );
                const actual = JSON.stringify(
                    details.links.map(({ value, target }) => ({
                        value,
                        target: target.replace(/ @[+-]?\d+$/, ""),
                    })),
                );
                if (expected !== actual) {
                    problems.push(`${at}: links ${expected} → ${actual}`);
                }
                linksCompared += links.length;
            } else {
                for (const link of links) {
                    if (link.spelled.replace(/^_+/, "") !== link.value) {
                        problems.push(`${at}: link ${link.value} reaches ${link.spelled}`);
                    }
                    linksSpelled += 1;
                }
            }
            const parameter = hint.label.endsWith(":") || hint.label.startsWith(", ");
            const kind = details?.kind ?? (parameter ? "Parameter" : "Type");
            if (hint.kind !== kind) {
                problems.push(`${at}: kind ${hint.kind}, ${kind} by the rule`);
            }
            const padding = details?.padding ?? (hint.label.endsWith(":") ? ["right"] : []);
            const old = [
                ...(hint.padding_left === true ? ["left"] : []),
                ...(hint.padding_right === true ? ["right"] : []),
            ];
            if (JSON.stringify(old) !== JSON.stringify(padding)) {
                problems.push(
                    `${at}: padding ${JSON.stringify(old)}, ${JSON.stringify(padding)} by the rule`,
                );
            }
        }
    }
}

let newAsked = 0;
for (const subtopic of fs.readdirSync(TOPIC, { withFileTypes: true })) {
    if (!subtopic.isDirectory()) {
        continue;
    }
    for (const name of fs.readdirSync(path.join(TOPIC, subtopic.name))) {
        const id = `${subtopic.name}/${name}`;
        if (!fs.existsSync(path.join(TOPIC, id, "snap.toml"))) {
            continue;
        }
        for (const blocks of Object.values(newOutput(id))) {
            newAsked += Object.keys(blocks).length * (SCENARIOS.has(id) ? 1 : 2);
        }
    }
}

console.log(
    `old: ${corpus.fixtures.length} fixtures (${UNPINNED.size} unpinned), ${oldAsked} requests, ${oldCount} hints`,
);
console.log(
    `links: ${linksCompared} compared where shown, ${linksSpelled} reach the name they link`,
);
console.log(`new: ${newAsked} requests across the inlay_hints topic`);
for (const line of added) {
    console.log(`added: ${line}`);
}
if (problems.length > 0) {
    console.log(`\n${problems.length} unexplained differences:\n  ${problems.join("\n  ")}`);
    process.exitCode = 1;
} else {
    console.log("no unexplained differences");
}
