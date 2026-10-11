/// One-off check of the code_completion migration: every item an old
/// snapshot of tests/snap/code_completion pinned is in the case it moved to,
/// rendered as the new output renders it, in the same rank.
///
///     node tools/migrate/code_completion.ts
///
/// An old item is its label and signature (`label`), kind, return type or
/// type (`detail`), deprecation, filter text, insert text and replaced
/// range. Where the new case does not show the replaced range, the old one
/// must be the identifier typed before the marker, which the cases showing
/// ranges pin; where it does not show the insert text, the old one must be
/// the label.

import * as fs from "node:fs";
import * as path from "node:path";
import yaml from "js-yaml";
import { snapCorpora, type SnapCorpus, type SnapFixture } from "../snap/corpus.ts";
import { parseSnap } from "../snap/snapshot.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { SourceText } from "../testing/render/position.ts";
import { readToml } from "../testing/render/value.ts";

const TOPIC = path.join(CASES_DIR, "completion");

/// Where an old fixture's markers went: the case, the new marker names, and
/// the new test each old test (`answers` for a fixture without config) is.
interface Target {
    case: string;
    markers?: Record<string, string>;
    tests?: Record<string, string>;
    /// A marker the new test asks on its own name, as `03_import_keyword`
    /// splits its markers between two tests.
    testOf?: Record<string, string>;
}

const MOVED: Record<string, Target> = {
    "defaulted_value_partial.cpp": { case: "templates/09_defaulted_value_partial" },
    "filtering_ranking/01_underscore_filter.cpp": { case: "filtering/01_underscore_filter" },
    "filtering_ranking/02_deprecated_tag.cpp": { case: "filtering/02_deprecated_tag" },
    "filtering_ranking/03_fuzzy_word_boundary.cpp": { case: "filtering/03_fuzzy_word_boundary" },
    "filtering_ranking/04_case_insensitive.cpp": { case: "filtering/04_case_insensitive" },
    "filtering_ranking/05_prefix_beats_subsequence.cpp": {
        case: "filtering/05_prefix_beats_subsequence",
    },
    "filtering_ranking/06_inside_a_word.cpp": { case: "filtering/06_inside_a_word" },
    "filtering_ranking/07_unicode_prefix.cpp": { case: "filtering/07_unicode_prefix" },
    "functions_snippets/01_function_candidates.cpp": {
        case: "functions_snippets/02_function_candidates",
    },
    "functions_snippets/02_overload_bundle.cpp": {
        case: "functions_snippets/03_overload_bundle",
        tests: { answers: "default" },
    },
    "functions_snippets/03_no_bundle_overloads.cpp": {
        case: "functions_snippets/03_overload_bundle",
    },
    "functions_snippets/04_snippet_arguments.cpp": {
        case: "functions_snippets/01_snippet_arguments",
    },
    "functions_snippets/05_snippet_bundle_mode.cpp": {
        case: "functions_snippets/04_snippet_bundle_mode",
    },
    "functions_snippets/06_default_argument.cpp": {
        case: "functions_snippets/05_parameter_details",
        markers: { pos: "default_argument" },
    },
    "functions_snippets/07_variadic_signature.cpp": {
        case: "functions_snippets/05_parameter_details",
        markers: { pos: "variadic" },
    },
    "functions_snippets/08_call_parentheses.cpp": {
        case: "functions_snippets/06_call_parentheses",
    },
    "functions_snippets/09_template_arguments.cpp": {
        case: "functions_snippets/07_template_arguments",
    },
    "functions_snippets/10_statement_snippets.cpp": {
        case: "functions_snippets/08_statement_snippets",
    },
    "include_path_completion/01_include_quoted.cpp": {
        case: "include_path_completion/01_include_quoted",
    },
    "include_path_completion/02_include_angled.cpp": {
        case: "include_path_completion/02_include_angled",
    },
    "include_path_completion/03_closing_delimiter.cpp": {
        case: "include_path_completion/03_closing_delimiter",
    },
    "member_access/01_member_access.cpp": {
        case: "member_access/02_member_labels",
        markers: { pos: "conversion" },
    },
    "member_access/02_member_template.cpp": {
        case: "member_access/02_member_labels",
        markers: { pos: "instantiated" },
    },
    "member_access/03_pointer_arrow.cpp": { case: "member_access/03_pointer_arrow" },
    "member_access/04_scope_access.cpp": { case: "member_access/04_scope_access" },
    "member_access/05_inherited_members.cpp": { case: "member_access/05_inherited_members" },
    "member_access/06_dependent_member_type.cpp": { case: "templates/01_dependent_member_type" },
    "member_access/07_partial_specialization_members.cpp": {
        case: "templates/02_partial_specialization",
    },
    "member_access/08_dependent_alias_chain.cpp": { case: "templates/03_alias_chain" },
    "member_access/09_dependent_pointee.cpp": {
        case: "templates/01_dependent_member_type",
        markers: { pos: "pointee" },
    },
    "member_access/10_concrete_alias_target.cpp": { case: "templates/04_concrete_alias_target" },
    "member_access/11_designated_initializer.cpp": {
        case: "member_access/06_designated_initializer",
    },
    "member_access/12_inaccessible_members.cpp": { case: "member_access/07_inaccessible_members" },
    "member_access/13_destructor_label.cpp": {
        case: "member_access/02_member_labels",
        markers: { pos: "namespaced" },
    },
    "member_access/14_dependent_expression_result.cpp": {
        case: "templates/05_dependent_expression_results",
    },
    "member_access/15_overloaded_subscript.cpp": { case: "templates/06_overloaded_subscript" },
    "member_access/16_deduced_variable.cpp": { case: "templates/07_deduced_variables" },
    "module_completion/01_import_modules/main.cpp": {
        case: "modules/01_import_modules",
        markers: { pos: "prefix" },
    },
    "module_completion/02_partition_imports/main.cpp": { case: "modules/02_partition_imports" },
    "module_completion/03_import_keyword/main.cpp": {
        case: "modules/03_import_keyword",
        testOf: { keyword: "keyword", declared: "module names" },
    },
    "module_completion/04_interface_partitions/main.cpp": {
        case: "modules/04_interface_partitions",
    },
    "object_qualifier_edges.cpp": { case: "templates/10_object_qualifiers" },
    "override_completion/01_override_declaration.cpp": {
        case: "overrides/01_override_declaration",
    },
    "own_specialization_member.cpp": { case: "templates/11_own_specialization" },
    "partial_specialization_prefix.cpp": { case: "templates/02_partial_specialization" },
    "recursive_auto_initializer.cpp": { case: "templates/12_unresolvable_objects" },
    "symbols/01_unqualified_lookup.cpp": { case: "symbols/01_unqualified_lookup" },
    "symbols/02_template_dedup.cpp": { case: "symbols/02_class_templates" },
    "symbols/03_constructor_labels.cpp": {
        case: "symbols/03_constructor_labels",
        markers: { pos: "class_template" },
    },
    "symbols/04_pattern_keyword.cpp": { case: "symbols/04_keywords" },
    "symbols/05_macros.cpp": { case: "symbols/05_macros" },
    "symbols/06_macro_shadow.cpp": { case: "symbols/06_macro_shadow" },
    "symbols/07_macro_argument.cpp": { case: "member_access/08_macro_argument" },
    "symbols/08_namespace_qualified.cpp": { case: "symbols/07_namespace_members" },
    "symbols/09_enum_members.cpp": { case: "symbols/08_enum_members" },
    "symbols/10_local_shadow.cpp": { case: "symbols/09_shadowing", markers: { pos: "variable" } },
    "symbols/11_using_declaration.cpp": { case: "symbols/10_using_declaration" },
    "symbols/12_dependent_scope.cpp": { case: "templates/08_dependent_scope" },
    "symbols/13_required_qualifier.cpp": { case: "symbols/11_required_qualifier" },
    "symbols/14_hidden_by_local.cpp": {
        case: "symbols/09_shadowing",
        markers: { pos: "function" },
    },
    "symbols/15_constructor_template.cpp": {
        case: "symbols/03_constructor_labels",
        markers: { pos: "constructor_template" },
    },
    "symbols/16_overloads_across_scopes.cpp": {
        case: "functions_snippets/09_overloads_across_scopes",
    },
    "unresolved_nested_subscript.cpp": {
        case: "templates/12_unresolvable_objects",
        markers: { pos: "nested_subscript" },
    },
};

/// The old paths a marker was asked on that the new case does not ask, and
/// why.
const NARROWED: Record<string, string> = {
    "module_completion/03_import_keyword/main.cpp declared":
        "inspect: clice inspect compiles one file alone, and the unit imports its module",
};

/// New tests (`case test`) asked on the server alone.
function serverOnly(id: string, test: string): boolean {
    const [subtopic, name] = id.split("/");
    if (subtopic === "include_path_completion" || subtopic === "triggers") {
        return true;
    }
    if (subtopic === "modules") {
        return !(name === "03_import_keyword" && test === "keyword");
    }
    return false;
}

/// Tests that start servers themselves: each block one request.
const SCENARIOS = new Set([
    "functions_snippets/10_client_snippets",
    "modules/06_import_in_buffer",
    "symbols/12_unsaved_edits",
]);

interface OldItem {
    label: string;
    kind?: string;
    filter?: string;
    detail?: string;
    description?: string;
    edit?: string;
    insert_range?: string;
    insert?: string;
    snippet?: boolean;
    deprecated?: boolean;
}

interface NewItem {
    label: string;
    kind: string;
    detail?: string;
    deprecated?: boolean;
    insert?: string;
    snippet?: boolean;
    filter?: string;
    replace?: string;
    insert_range?: string;
}

interface NewAnswer {
    items: NewItem[];
    more?: number;
}

function corpusOf(feature: string): SnapCorpus {
    const found = snapCorpora().find((c) => c.feature === feature);
    if (found === undefined) {
        throw new Error(`no ${feature} corpus`);
    }
    return found;
}

const corpus = corpusOf("code_completion");

/// The old snapshot as `test → marker → items` (a fixture without config
/// has one test, `answers`), and the `… +N more` counts.
function oldAnswers(
    fixture: SnapFixture,
): Map<string, Map<string, { items: OldItem[]; more: number }>> {
    const file = path.join(corpus.corpus, fixture.rel.replace(/\.cpp$/, ".snap.yml"));
    const snap = parseSnap(fs.readFileSync(file, "utf8"));
    if (snap === null) {
        throw new Error(`${file}: no front matter`);
    }
    const body = (yaml.load(snap.body) ?? {}) as Record<string, unknown>;
    const tests: Record<string, Record<string, unknown>> = fixture.meta.config === undefined
        ? { answers: body }
        : (body as Record<string, Record<string, unknown>>);
    const out = new Map<string, Map<string, { items: OldItem[]; more: number }>>();
    for (const [test, markers] of Object.entries(tests)) {
        const byMarker = new Map<string, { items: OldItem[]; more: number }>();
        for (const [marker, list] of Object.entries(markers)) {
            const items: OldItem[] = [];
            let more = 0;
            for (const entry of (list ?? []) as unknown[]) {
                if (typeof entry === "string") {
                    more = Number(/\+(\d+) more/.exec(entry)?.[1] ?? 0);
                    continue;
                }
                items.push(entry as OldItem);
            }
            byMarker.set(marker, { items, more });
        }
        out.set(test, byMarker);
    }
    return out;
}

const readCases = new Map<string, Record<string, Record<string, unknown>>>();

function newOutput(id: string): Record<string, Record<string, unknown>> {
    let found = readCases.get(id);
    if (found === undefined) {
        const file = path.join(TOPIC, id, "snap.toml");
        found = readToml(fs.readFileSync(file, "utf8")) as Record<string, Record<string, unknown>>;
        readCases.set(id, found);
    }
    return found;
}

function newAnswer(id: string, test: string, label: string): NewAnswer | undefined {
    const block = newOutput(id)[test]?.[label] as { completion?: NewAnswer } | undefined;
    return block?.completion;
}

/// `L:C-L:C` of `text` rendered as the new output renders a range in the
/// file a request asked: its line with the range in ⟦⟧, indentation dropped.
function renderRange(text: SourceText, range: string): string {
    const [start, end] = range.split("-").map((point) => {
        const [line, character] = point.split(":").map(Number);
        return { line: line ?? 0, character: character ?? 0 };
    });
    if (start?.line === undefined || start.line !== end?.line) {
        throw new Error(`a one-line range: ${range}`);
    }
    const line = text.line(start.line);
    const marked = `${line.slice(0, start.character)}⟦${line.slice(start.character, end.character)}⟧${line.slice(end.character)}`;
    return marked.trim();
}

/// Without the offset telling repeated lines apart: line numbers moved.
function bare(rendered: string): string {
    return rendered.replace(/ @[+-]?\d+$/, "");
}

const IDENTIFIER = /[A-Za-z0-9_\u0080-￿]/;

/// The identifier typed before `offset`: what a completion there replaces.
function typedRange(text: SourceText, offset: number): string {
    let begin = offset;
    while (begin > 0 && IDENTIFIER.test(text.text[begin - 1] ?? "")) {
        begin -= 1;
    }
    const from = text.positionAt(begin);
    const to = text.positionAt(offset);
    return `${from.line}:${from.character}-${to.line}:${to.character}`;
}

const problems: string[] = [];
let oldAsked = 0;
let oldItems = 0;
let checked = 0;
let implied = 0;
const narrowed: string[] = [];
/// Answers whose items tie in rank and order by another rule now.
const reordered: string[] = [];

for (const fixture of corpus.fixtures) {
    const target = MOVED[fixture.rel];
    if (target === undefined) {
        problems.push(`${fixture.rel}: no target`);
        continue;
    }
    const paths = fixture.meta.verify === "both" ? 2 : 1;
    const entry = fixture.files.find((file) => file.rel === fixture.rel);
    if (entry === undefined) {
        throw new Error(`${fixture.rel}: no entry`);
    }
    const text = new SourceText(fixture.rel, entry.source.content);
    for (const [test, markers] of oldAnswers(fixture)) {
        for (const [marker, old] of markers) {
            oldAsked += paths;
            const label = target.markers?.[marker] ?? marker;
            const newTest = target.testOf?.[marker] ?? target.tests?.[test] ?? test;
            const where = `${fixture.rel} ${test}.${marker} → ${target.case} ${newTest}.${label}`;
            const narrow = NARROWED[`${fixture.rel} ${marker}`];
            if (narrow !== undefined) {
                narrowed.push(`${where}: ${narrow}`);
            } else if (paths === 2 && serverOnly(target.case, newTest)) {
                problems.push(`${where}: asked on the server alone`);
            }
            const now = newAnswer(target.case, newTest, label);
            if (now === undefined) {
                problems.push(`${where}: no such block`);
                continue;
            }
            if (old.items.length !== now.items.length || old.more !== (now.more ?? 0)) {
                problems.push(
                    `${where}: ${old.items.length} items +${old.more} before, ${now.items.length} +${now.more ?? 0} now`,
                );
                continue;
            }
            const offset = entry.source.offsets.get(marker);
            const expected: string[] = [];
            const actual: string[] = [];
            old.items.forEach((item, index) => {
                oldItems += 1;
                const shown = now.items[index];
                const at = `${where} ${item.label}`;
                const fact: Record<string, unknown> = {
                    label: item.label + (item.detail ?? ""),
                    kind: item.kind,
                    detail: item.description,
                    deprecated: item.deprecated === true,
                };
                if (item.filter !== undefined) {
                    fact["filter"] = item.filter;
                }
                // Every item of a new answer shows the same parts.
                if (shown?.insert !== undefined) {
                    fact["insert"] = item.insert ?? item.label;
                    fact["snippet"] = item.snippet === true;
                } else if (item.insert !== undefined) {
                    problems.push(`${at}: insert ${item.insert} not shown`);
                }
                if (shown?.replace !== undefined && item.edit !== undefined) {
                    fact["replace"] = renderRange(text, item.edit);
                    fact["insert_range"] =
                        item.insert_range === undefined
                            ? undefined
                            : renderRange(text, item.insert_range);
                } else if (item.edit === undefined) {
                    problems.push(`${at}: no replaced range before`);
                } else if (offset !== undefined && item.edit === typedRange(text, offset)) {
                    implied += 1;
                    if (item.insert_range !== undefined) {
                        problems.push(`${at}: insert range ${item.insert_range} not shown`);
                    }
                } else {
                    problems.push(`${at}: replaced range ${item.edit} not shown`);
                }
                expected.push(JSON.stringify(fact));
                checked += 1;
            });
            for (const item of now.items) {
                const fact: Record<string, unknown> = {
                    label: item.label,
                    kind: item.kind,
                    detail: item.detail,
                    deprecated: item.deprecated === true,
                };
                const source = old.items[actual.length];
                if (source?.filter !== undefined) {
                    fact["filter"] = item.filter;
                }
                if (item.insert !== undefined) {
                    fact["insert"] = item.insert;
                    fact["snippet"] = item.snippet === true;
                }
                if (item.replace !== undefined) {
                    fact["replace"] = bare(item.replace);
                    fact["insert_range"] =
                        item.insert_range === undefined ? undefined : bare(item.insert_range);
                }
                actual.push(JSON.stringify(fact));
            }
            if (expected.join("\n") === actual.join("\n")) {
                continue;
            }
            if ([...expected].sort().join("\n") === [...actual].sort().join("\n")) {
                reordered.push(`${where}: ${expected.length} items tied in rank`);
                continue;
            }
            const missing = expected.filter((fact) => !actual.includes(fact));
            const extra = actual.filter((fact) => !expected.includes(fact));
            problems.push(
                `${where}:\n    before only ${missing.join("\n    before only ")}\n    now only ${extra.join("\n    now only ")}`,
            );
        }
    }
}

/// What the new cases ask: per block, one request on each client running
/// its test.
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
        for (const [test, blocks] of Object.entries(newOutput(id))) {
            const clients = SCENARIOS.has(id) || serverOnly(id, test) ? 1 : 2;
            newAsked += Object.keys(blocks).length * clients;
        }
    }
}

console.log(`old: ${corpus.fixtures.length} fixtures, ${oldAsked} requests, ${oldItems} items`);
console.log(`checked ${checked} items; ${implied} replaced ranges are the typed identifier`);
console.log(`new: ${newAsked} requests across the completion topic`);
for (const line of narrowed) {
    console.log(`narrowed: ${line}`);
}
for (const line of reordered) {
    console.log(`reordered: ${line}`);
}
if (problems.length > 0) {
    console.log(`\n${problems.length} unexplained differences:\n  ${problems.join("\n  ")}`);
    process.exitCode = 1;
} else {
    console.log("no unexplained differences");
}
