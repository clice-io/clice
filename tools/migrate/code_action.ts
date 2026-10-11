/// One-off check of the code_action migration: every action an old snapshot
/// of tests/snap/code_action pinned is in the case it moved to, with its
/// kind, its title and what its edits do, rendered as the new output renders
/// them, a diff of each edited file.
///
///     node tools/migrate/code_action.ts
///
/// A fixture whose code the migration changed (merged with an integration
/// test, its stand-in headers renamed) names why in CHANGED; its edits are
/// then compared by the lines they change rather than by the whole diff.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "../snap/annotation.ts";
import { snapCorpora, type SnapFixture } from "../snap/corpus.ts";
import { parseSnap } from "../snap/snapshot.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { renderDiff } from "../testing/render/edits.ts";
import { readToml } from "../testing/render/value.ts";

const TOPIC = path.join(CASES_DIR, "code_actions");

/// Where an old fixture's markers went: the case, and the marker names that
/// changed on the way.
interface Target {
    case: string;
    labels?: Record<string, string>;
    /// Markers that moved to another case.
    moved?: Record<string, { case: string; label: string }>;
}

const MOVED: Record<string, Target> = {
    "constrained_copy.cpp": { case: "constructor/08_constrained_copy" },
    "constructor/01_memberwise.cpp": { case: "constructor/01_memberwise" },
    "constructor/02_existing_constructor.cpp": { case: "constructor/02_existing_constructor" },
    "constructor/03_single_field.cpp": { case: "constructor/03_single_field" },
    "constructor/04_declarator_fields.cpp": { case: "constructor/04_declarator_fields" },
    "constructor/05_base_without_default.cpp": { case: "constructor/05_base_without_default" },
    "constructor/06_implicitly_deleted_base.cpp": {
        case: "constructor/06_implicitly_deleted_base",
    },
    "constructor/07_move_only_fields.cpp": { case: "constructor/07_move_only_fields" },
    "decltype_declared_type.cpp": { case: "deduced_type/09_decltype_declared_type" },
    "deduced_type/01_auto_variable.cpp": {
        case: "deduced_type/01_auto_declarations",
        labels: { plain: "number", ref: "reference", nested: "reference" },
    },
    "deduced_type/02_auto_return.cpp": { case: "deduced_type/02_auto_return" },
    "deduced_type/03_decltype.cpp": { case: "deduced_type/03_decltype" },
    "deduced_type/04_unnameable_types.cpp": { case: "deduced_type/04_unnameable_types" },
    "deduced_type/05_forwarding_reference.cpp": { case: "deduced_type/05_forwarding_reference" },
    "deduced_type/06_shadowed_names.cpp": { case: "deduced_type/06_shadowed_names" },
    "deduced_type/07_standard_names.cpp": {
        case: "deduced_type/13_standard_names_checked",
        labels: { size: "size_std", declared_null: "null_std" },
    },
    "deduced_type/08_const_pointer.cpp": { case: "deduced_type/08_const_pointer" },
    "define/01_method_out_of_line.cpp": { case: "define/01_method_out_of_line" },
    "define/02_declaration_specifiers.cpp": { case: "define/02_declaration_specifiers" },
    "define/03_qualified_return_type.cpp": { case: "define/03_qualified_return_type" },
    "define/04_class_template_member.cpp": { case: "define/04_class_template_member" },
    "define/05_member_template.cpp": { case: "define/05_member_template" },
    "define/06_special_members.cpp": { case: "define/06_special_members" },
    "define/07_free_function.cpp": { case: "define/07_free_function" },
    "define/08_placement_after_definitions.cpp": {
        case: "define/08_placement_after_definitions",
    },
    "define/09_missing_members.cpp": { case: "define/09_missing_members" },
    "define/10_missing_from_definition.cpp": { case: "define/10_missing_from_definition" },
    "define/11_header_host/main.cpp": { case: "define/11_host_source" },
    "define/12_nested_class.cpp": { case: "define/12_nested_class" },
    "define/13_dependent_return_type.cpp": { case: "define/13_dependent_return_type" },
    "define/14_linkage_specification.cpp": { case: "define/14_linkage_specification" },
    "fix/01_compiler_fix.cpp": { case: "fix/01_compiler_fix" },
    "fix/02_tidy_fix.cpp": { case: "fix/02_tidy_fix" },
    "fix/03_note_fixes.cpp": { case: "fix/03_note_fixes" },
    "implement/01_pure_virtuals.cpp": { case: "implement/01_pure_virtuals" },
    "implement/02_inheritance_chain.cpp": { case: "implement/02_inheritance_chain" },
    "implement/03_qualified_types.cpp": { case: "implement/03_qualified_types" },
    "implement/04_conversion_and_pointers.cpp": { case: "implement/04_conversion_and_pointers" },
    "implement/05_shared_signatures.cpp": { case: "implement/05_shared_signatures" },
    "implement/06_specifiers.cpp": { case: "implement/06_specifiers" },
    "include/01_standard_library.cpp": {
        case: "include/01_standard_library",
        moved: { printf: { case: "include/12_unqualified_standard_name", label: "printf" } },
    },
    "include/02_project_header/main.cpp": { case: "include/02_project_header" },
    "include/03_no_include_yet.cpp": { case: "include/03_no_include_yet" },
    "include/04_conditional_includes.cpp": { case: "include/04_conditional_includes" },
    "include/05_trailing_includes.cpp": { case: "include/05_trailing_includes" },
    "include_feature_macro_block.cpp": { case: "include/06_feature_macro_block" },
    "include_guard_comments.cpp": { case: "include/08_guard_comments" },
    "include_member_access.cpp": { case: "include/09_member_access" },
    "include_module_namespace.cpp": { case: "include/10_module_namespace" },
    "include_module_unit.cpp": { case: "include/11_module_unit" },
    "invalid_classes.cpp": { case: "constructor/09_invalid_classes" },
    "macro/01_expand_macro.cpp": { case: "macro/01_expand_macro" },
    "macro/02_nested_expansion.cpp": { case: "macro/02_nested_expansion" },
    "macro/03_directives_and_empty.cpp": { case: "macro/03_directives_and_empty" },
    "macro/04_pragma_operator.cpp": { case: "macro/04_pragma_operator" },
    "macro/05_token_boundaries.cpp": { case: "macro/05_token_boundaries" },
    "missing_from_header/main.cpp": { case: "define/17_header_declared_class" },
    "move_constructor_template.cpp": { case: "constructor/10_move_constructor_template" },
    "move_declared_later.cpp": { case: "constructor/11_move_declared_later" },
    "msvc_implicit_size_t.cpp": { case: "deduced_type/10_msvc_implicit_size_t" },
    "reorder/01_class_members.cpp": { case: "reorder/01_class_members" },
    "reorder/02_namespace_blocks.cpp": { case: "reorder/02_namespace_blocks" },
    "reorder/03_free_functions.cpp": { case: "reorder/03_free_functions" },
    "reorder/04_trailing_comments.cpp": { case: "reorder/04_trailing_comments" },
    "reorder/05_dependencies.cpp": { case: "reorder/05_dependencies" },
    "reorder_dependency_kinds.cpp": { case: "reorder/06_dependency_kinds" },
    "reorder_leading_attributes.cpp": { case: "reorder/07_leading_attributes" },
    "reorder_preamble_conditional.cpp": { case: "reorder/08_preamble_conditional" },
    "reorder_unterminated_last_line.cpp": { case: "reorder/10_unterminated_last_line" },
    "switch_cases/01_missing_cases.cpp": { case: "switch_cases/01_missing_cases" },
    "switch_cases/02_before_default.cpp": { case: "switch_cases/02_before_default" },
    "switch_cases/03_unscoped_enum.cpp": { case: "switch_cases/03_unscoped_enum" },
    "switch_cases/04_complete_switch.cpp": { case: "switch_cases/04_complete_switch" },
    "switch_cases/05_selection_range.cpp": { case: "switch_cases/05_selection_range" },
    "switch_cases/06_dependent_labels.cpp": { case: "switch_cases/06_dependent_labels" },
    "switch_cases/07_declaring_section.cpp": { case: "switch_cases/07_declaring_section" },
    "switch_declaring_body.cpp": { case: "switch_cases/08_declaring_body" },
    "template_auto_parameter.cpp": { case: "define/15_auto_template_parameters" },
};

/// Fixtures whose code changed on the way, and why: their edits are
/// compared by the lines they change, under `rename` from old to new text.
const CHANGED: Record<
    string,
    { why: string; rename?: [string, string][]; unchecked?: Record<string, string> }
> = {
    "deduced_type/01_auto_variable.cpp": {
        why: "01_auto_declarations was ported before this migration with code of its own: number pins plain, pointer was added for pointer, and reference pins ref and nested at once",
        rename: [
            ["auto x = 1", "auto tonnes = 12"],
            ["int x = 1", "int tonnes = 12"],
            ["auto* z = &x", "auto* heaviest = &tonnes"],
            ["int* z = &x", "int* heaviest = &tonnes"],
        ],
        unchecked: {
            ref: "a reference to a nested type of a namespace, fleet::Truck::Load, where the old fixture asked a reference to int",
            nested: "the same nested type, reached through a reference",
        },
    },
    "deduced_type/07_standard_names.cpp": {
        why: "merged with the spelling test of the same names, which checks them with static_asserts; the documented case now takes the names from an included header, as real code does",
        rename: [
            ["auto null = nullptr", "auto p = nullptr"],
            ["decltype(nullptr) null = nullptr", "decltype(nullptr) p = nullptr"],
            ["std::nullptr_t null = nullptr", "std::nullptr_t p = nullptr"],
            ["auto size = sizeof(int)", "auto n = sizeof(int)"],
            ["std::size_t size = sizeof(int)", "std::size_t n = sizeof(int)"],
            ["auto difference = last - first", "auto d = b - a"],
            ["std::ptrdiff_t difference = last - first", "std::ptrdiff_t d = b - a"],
        ],
    },
    "deduced_type/08_const_pointer.cpp": {
        why: "merged with the spelling test of cv-qualified pointers: static_asserts after each declaration, the alias, macro, comment and hidden markers",
    },
    "decltype_declared_type.cpp": {
        why: "merged with the spelling test of the same code: static_asserts after each declaration",
    },
    "implement/05_shared_signatures.cpp": {
        why: "merged with the spelling test of shared signatures: a static_assert after the class",
    },
    "implement/06_specifiers.cpp": {
        why: "merged with the spelling test of override specifiers: a static_assert after the class",
    },
    "include/01_standard_library.cpp": {
        why: "printf moved to a case of its own, so that each action's result compiles; support.h became config.h",
        rename: [['"support.h"', '"config.h"']],
    },
    "include/04_conditional_includes.cpp": {
        why: "support.h became config.h",
        rename: [['"support.h"', '"config.h"']],
    },
    "include/05_trailing_includes.cpp": {
        why: "merged with the integration test of embedded and trailing includes: the includes name real files",
        rename: [['"support.h"', '"config.h"']],
    },
    "include_feature_macro_block.cpp": {
        why: "support.h became config.h",
        rename: [['"support.h"', '"config.h"']],
    },
    "macro/05_token_boundaries.cpp": {
        why: "merged with the integration test of the same macros: constexpr operands and static_asserts check the meaning",
        rename: [
            ["int negated = NEG-a;", "static_assert(NEG-value == 4);"],
            ["int negated = - -a;", "static_assert(- -value == 4);"],
            ["int divided = a/DEREF(p);", "static_assert(12/DEREF(pointer) == 3);"],
            ["int divided = a/ *p;", "static_assert(12/ *pointer == 3);"],
            ["int picked = PICK(c);", "static_assert(PICK(false) == 2);"],
            [
                "int picked = (c ? 1 : ::fallback());",
                "static_assert((false ? 1 : ::fallback()) == 2);",
            ],
            ["int removed = a/NOTHING*p;", "static_assert(12/NOTHING*pointer == 3);"],
            ["int removed = a/ *p;", "static_assert(12/ *pointer == 3);"],
        ],
    },
};

interface Action {
    kind: string;
    title: string;
    /// The text of each edited file before and after the action.
    files: Map<string, { before: string; after: string }>;
    host: string[];
}

/// The leading comments and blank lines of a file: the old fixture's doc
/// header or the new case's note.
function lead(text: string): string[] {
    const out: string[] = [];
    for (const line of text.split("\n")) {
        if (!line.startsWith("//") && line.trim() !== "") {
            break;
        }
        out.push(line);
    }
    return out;
}

/// `text` without the run of lines `block` names, wherever an edit moved it.
function without(text: string, block: string[]): string {
    if (block.length === 0) {
        return text;
    }
    const lines = text.split("\n");
    for (let i = 0; i + block.length <= lines.length; i++) {
        if (block.every((line, j) => lines[i + j] === line)) {
            lines.splice(i, block.length);
            return lines.join("\n");
        }
    }
    return text;
}

function offsetOf(text: string, line: number, character: number): number {
    let offset = 0;
    for (let i = 0; i < line; i++) {
        offset = text.indexOf("\n", offset) + 1;
    }
    return offset + character;
}

function unquote(raw: string): string {
    return JSON.parse(raw.replace(/\\x([0-9a-f]{2})/g, "\\u00$1")) as string;
}

/// An old fixture's sources, markers stripped, by fixture-relative path.
function oldSources(fixture: SnapFixture): Map<string, string> {
    const out = new Map<string, string>();
    for (const file of fixture.files) {
        const rel = fixture.unit === "" ? "main.cpp" : file.rel.slice(fixture.unit.length + 1);
        out.set(rel, file.source.content);
    }
    return out;
}

/// The fixture-relative file each marker of an old fixture is in.
function markerFiles(fixture: SnapFixture): Map<string, string> {
    const out = new Map<string, string>();
    for (const file of fixture.files) {
        const rel = fixture.unit === "" ? "main.cpp" : file.rel.slice(fixture.unit.length + 1);
        for (const name of [...file.source.offsets.keys(), ...file.source.ranges.keys()]) {
            out.set(name, rel);
        }
    }
    return out;
}

/// The old snapshot's actions by marker.
function oldActions(fixture: SnapFixture, file: string): Map<string, Action[]> {
    const sources = oldSources(fixture);
    const marked = markerFiles(fixture);
    let requested = "main.cpp";
    const snap = parseSnap(fs.readFileSync(file, "utf8"));
    if (snap === null) {
        throw new Error(`${file}: no front matter`);
    }
    const out = new Map<string, Action[]>();
    let actions: Action[] = [];
    let current: Action | undefined;
    let edits: { file: string; begin: number; end: number; text: string }[] = [];
    const close = (): void => {
        if (current === undefined) {
            return;
        }
        const byFile = new Map<string, typeof edits>();
        for (const edit of edits) {
            byFile.set(edit.file, [...(byFile.get(edit.file) ?? []), edit]);
        }
        for (const [rel, list] of byFile) {
            const before = sources.get(rel) ?? "";
            let after = before;
            for (const edit of [...list].sort((a, b) => b.begin - a.begin)) {
                after = after.slice(0, edit.begin) + edit.text + after.slice(edit.end);
            }
            current.files.set(rel, { before, after });
        }
        actions.push(current);
        current = undefined;
        edits = [];
    };
    let inHost = false;
    for (const line of snap.body.split("\n")) {
        const section = /^(\S+):$/.exec(line);
        if (section !== null) {
            close();
            actions = [];
            out.set(section[1] ?? "", actions);
            requested = marked.get(section[1] ?? "") ?? "main.cpp";
            continue;
        }
        const head = /^- \[([^\]]+)\] (.*)$/.exec(line);
        if (head !== null) {
            close();
            current = { kind: head[1] ?? "", title: head[2] ?? "", files: new Map(), host: [] };
            inHost = false;
            continue;
        }
        if (line === "  into the host source:") {
            inHost = true;
            continue;
        }
        if (inHost && line.startsWith("    ")) {
            current?.host.push(unquote(line.trim()));
            continue;
        }
        const edit = /^ {2}(?:\$\{WS\}\/\S+\/(\S+) )?(\d+):(\d+)-(\d+):(\d+) => (".*")$/.exec(line);
        if (edit !== null) {
            const rel = edit[1] ?? requested;
            const text = sources.get(rel) ?? "";
            const [l1, c1, l2, c2] = [2, 3, 4, 5].map((i) => Number(edit[i]));
            edits.push({
                file: rel,
                begin: offsetOf(text, l1 ?? 0, c1 ?? 0),
                end: offsetOf(text, l2 ?? 0, c2 ?? 0),
                text: unquote(edit[6] ?? '""'),
            });
        }
    }
    close();
    return out;
}

/// Apply a rendered diff back to the text it was rendered from.
function applyDiff(before: string, diff: string): string {
    if (diff === "(no change)") {
        return before;
    }
    const restore = (line: string): string =>
        line.replace(/[·→]+$/, (run) => run.replaceAll("·", " ").replaceAll("→", "\t"));
    const hunks: { op: string; text: string; bare: boolean }[][] = [[]];
    for (const line of diff.split("\n")) {
        if (line === "…") {
            hunks.push([]);
            continue;
        }
        const hunk = hunks[hunks.length - 1] ?? [];
        if (line.startsWith("\\")) {
            const last = hunk[hunk.length - 1];
            if (last !== undefined) {
                last.bare = true;
            }
            continue;
        }
        const op = line === "" ? " " : line.charAt(0);
        hunk.push({ op, text: restore(line.slice(1)), bare: false });
    }
    const lines = before.split("\n");
    const ended = before.endsWith("\n");
    if (ended) {
        lines.pop();
    }
    let at = 0;
    const out: string[] = [];
    let lastBare = !ended;
    for (const hunk of hunks) {
        const old = hunk.filter((entry) => entry.op !== "+").map((entry) => entry.text);
        let start = at;
        while (start <= lines.length - old.length) {
            if (old.every((line, i) => lines[start + i] === line)) {
                break;
            }
            start += 1;
        }
        if (start > lines.length - old.length) {
            throw new Error(`a hunk does not apply:\n${diff}`);
        }
        out.push(...lines.slice(at, start));
        for (const entry of hunk) {
            if (entry.op !== "-") {
                out.push(entry.text);
                lastBare = entry.bare;
            }
        }
        at = start + old.length;
    }
    if (at < lines.length) {
        out.push(...lines.slice(at));
        lastBare = !ended;
    }
    return out.join("\n") + (lastBare ? "" : "\n");
}

function readSource(dir: string, rel: string): string {
    return parseAnnotations(fs.readFileSync(path.join(dir, rel), "utf8")).content;
}

/// A new case's actions by marker, for the test (and client) given.
function newActions(caseId: string, keys: string[]): Map<string, Action[]> {
    const dir = path.join(TOPIC, caseId);
    let table = readToml(fs.readFileSync(path.join(dir, "snap.toml"), "utf8"));
    for (const key of keys) {
        table = table[key] as Record<string, unknown>;
    }
    const out = new Map<string, Action[]>();
    for (const [label, block] of Object.entries(table)) {
        const list = (block as { code_actions?: unknown[] }).code_actions;
        if (list === undefined) {
            continue;
        }
        out.set(
            label,
            list.map((raw) => {
                const entry = raw as {
                    kind: string;
                    title: string;
                    edits?: Record<string, string>;
                    host?: string[];
                };
                const files = new Map<string, { before: string; after: string }>();
                for (const [rel, diff] of Object.entries(entry.edits ?? {})) {
                    const before = readSource(dir, rel);
                    files.set(rel, { before, after: applyDiff(before, diff) });
                }
                return { kind: entry.kind, title: entry.title, files, host: entry.host ?? [] };
            }),
        );
    }
    return out;
}

/// What an edit does, its header or note left out: the rendered diff, and
/// the lines it changes.
function rendered(rel: string, file: { before: string; after: string }) {
    const block = lead(file.before);
    const before = without(file.before, block);
    const after = without(file.after, block);
    const diff = renderDiff(rel, before, after);
    const changed = diff
        .split("\n")
        .filter((line) => line.startsWith("+") || line.startsWith("-"))
        .map((line) => line.charAt(0) + line.slice(1).trim())
        .sort();
    return { diff, changed };
}

function renamed(text: string, rename: [string, string][]): string {
    let out = text;
    for (const [from, to] of rename) {
        out = out.replaceAll(from, to);
    }
    return out;
}

const problems: string[] = [];
const explained = new Map<string, string>();
let facts = 0;
let oldAsked = 0;

const code = snapCorpora().find((c) => c.feature === "code_action");
if (code === undefined) {
    throw new Error("no code_action corpus");
}
for (const fixture of code.fixtures) {
    const target = MOVED[fixture.rel];
    if (target === undefined) {
        problems.push(`${fixture.rel}: no case`);
        continue;
    }
    const base = path.join(code.corpus, fixture.rel.replace(/\.cpp$/, ""));
    const variants: [string, string[]][] =
        fixture.meta.snap === "separate"
            ? [
                  [`${base}.inspect.snap.yml`, ["answers", "inspect"]],
                  [`${base}.server.snap.yml`, ["answers", "server"]],
              ]
            : [[`${base}.snap.yml`, ["answers"]]];
    const changed = CHANGED[fixture.rel];
    for (const [file, keys] of variants) {
        const old = oldActions(fixture, file);
        oldAsked += old.size * (variants.length === 1 ? 2 : 1);
        for (const [label, actions] of old) {
            const moved = target.moved?.[label];
            const caseId = moved?.case ?? target.case;
            const name = moved?.label ?? target.labels?.[label] ?? label;
            const where = `${fixture.rel} ${keys.slice(1).join("")} ${label} -> ${caseId} ${name}`;
            const unchecked = changed?.unchecked?.[label];
            if (unchecked !== undefined) {
                explained.set(`${fixture.rel} ${label}`, `${changed?.why ?? ""}: ${unchecked}`);
                continue;
            }
            const found = newActions(caseId, keys).get(name);
            if (found === undefined) {
                problems.push(`${where}: no block`);
                continue;
            }
            if (found.length !== actions.length) {
                problems.push(`${where}: ${actions.length} actions, now ${found.length}`);
                continue;
            }
            if (actions.length === 0) {
                facts += 1;
            }
            actions.forEach((action, index) => {
                facts += 1;
                const now = found[index];
                const rename = changed?.rename ?? [];
                if (now === undefined) {
                    return;
                }
                if (renamed(action.title, rename) !== now.title || action.kind !== now.kind) {
                    problems.push(`${where}: '${action.title}' is now '${now.title}'`);
                    return;
                }
                // snap.toml writes each piece as a block of lines, its final
                // newline that block's end.
                const pieces = (host: string[]): string =>
                    JSON.stringify(host.map((piece) => piece.replace(/\n$/, "")));
                if (pieces(action.host) !== pieces(now.host)) {
                    problems.push(
                        `${where}: host definitions differ: ${JSON.stringify(action.host)} vs ${JSON.stringify(now.host)}`,
                    );
                }
                for (const [rel, edit] of action.files) {
                    const other = now.files.get(rel);
                    if (other === undefined) {
                        problems.push(`${where}: ${rel} is no longer edited`);
                        continue;
                    }
                    const was = rendered(rel, edit);
                    const is = rendered(rel, other);
                    if (was.diff === is.diff) {
                        continue;
                    }
                    const lines = was.changed
                        .map((line) => line.charAt(0) + renamed(line.slice(1), rename))
                        .sort();
                    if (
                        changed !== undefined &&
                        JSON.stringify(lines) === JSON.stringify(is.changed)
                    ) {
                        explained.set(fixture.rel, changed.why);
                        continue;
                    }
                    problems.push(
                        `${where}: ${rel} differs\n--- was\n${was.diff}\n--- now\n${is.diff}`,
                    );
                }
            });
        }
    }
}

console.log(`${code.fixtures.length} fixtures, ${facts} actions and empty answers checked`);
console.log(`old: ${oldAsked} requests (markers x paths)`);
for (const [rel, why] of explained) {
    console.log(`changed on purpose: ${rel}: ${why}`);
}
if (problems.length > 0) {
    console.log(`\n${problems.length} unexplained differences:\n${problems.join("\n")}`);
    process.exitCode = 1;
} else {
    console.log("no unexplained differences");
}
