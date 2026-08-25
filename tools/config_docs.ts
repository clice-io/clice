/// Render the configuration reference from the committed JSON schema.
///
/// The schema (docs/public/clice-config.schema.json) is dumped by
/// `clice inspect --config-schema` from the annotated config structs —
/// the single source of truth for option names, types, defaults and
/// descriptions. This script rewrites the generated regions of
/// docs/en/guide/configuration.md from it:
///
///     <!-- BEGIN GENERATED CONFIG: project -->
///     <!-- END GENERATED CONFIG -->
///
/// Section intros, the file-location/precedence preamble, variable
/// substitution and the example stay handwritten. CI runs `check`; a
/// separate binary-backed step keeps the committed schema itself fresh.
///
/// Usage:
///     node tools/config_docs.ts update   # rewrite generated regions
///     node tools/config_docs.ts check    # fail if regions are stale

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "./compile_commands.ts";

const SCHEMA_PATH = "docs/public/clice-config.schema.json";
const DOC_PATH = "docs/en/guide/configuration.md";

const BEGIN_RE = /^<!-- BEGIN GENERATED CONFIG: (.+?) -->$/;
const END_MARKER = "<!-- END GENERATED CONFIG -->";

interface FieldSchema {
    type?: string;
    items?: FieldSchema;
    minimum?: number;
    maximum?: number;
    description?: string;
    default?: unknown;
    $ref?: string;
}

interface StructSchema {
    properties?: Record<string, FieldSchema>;
    $defs?: Record<string, StructSchema>;
}

function resolveRef(root: StructSchema, schema: FieldSchema): StructSchema {
    const ref = schema.$ref;
    if (ref === undefined) {
        return schema as StructSchema;
    }
    const prefix = "#/$defs/";
    if (!ref.startsWith(prefix)) {
        throw new Error(`unsupported $ref '${ref}'`);
    }
    const target = root.$defs?.[ref.slice(prefix.length)];
    if (!target) {
        throw new Error(`dangling $ref '${ref}'`);
    }
    return target;
}

/// The doc spelling of a field's type, mirroring the C++ declaration
/// (`uint32`/`uint64` from the integer bounds, since JSON schema has no
/// width of its own).
function renderType(field: FieldSchema): string {
    switch (field.type) {
        case "boolean":
            return "`bool`";
        case "string":
            return "`string`";
        case "integer": {
            // The exact maximum decides the width — a maximum this table
            // does not know must fail loudly rather than render a wrong
            // type name. The minimum may sit above the width's floor when
            // the field carries a validity bound (zero-invalid fields).
            if (field.minimum === undefined || field.maximum === undefined) {
                throw new Error(`missing integer bounds [${field.minimum}, ${field.maximum}]`);
            }
            if (field.minimum >= 0 && field.maximum === 4294967295) {
                return "`uint32`";
            }
            if (field.minimum >= 0 && field.maximum > 2 ** 53) {
                return "`uint64`";
            }
            throw new Error(`unhandled integer bounds [${field.minimum}, ${field.maximum}]`);
        }
        case "array": {
            if (field.items?.type === "string") {
                return "`array of string`";
            }
            if (field.items?.$ref !== undefined) {
                return "`array of table`";
            }
            throw new Error(`unhandled array item schema '${JSON.stringify(field.items)}'`);
        }
        default:
            throw new Error(`unhandled schema type '${field.type}'`);
    }
}

/// The Default cell. A field without a `default` annotation derives its
/// value from the running machine — the description states how — so the
/// cell shows a dash rather than any one host's number.
function renderDefault(field: FieldSchema): string {
    if (!("default" in field)) {
        return "—";
    }
    return `\`${JSON.stringify(field.default)}\``;
}

/// Prettier pads table columns to equal width; match it so `pixi run
/// format` leaves the generated regions untouched.
function renderTable(rows: string[][]): string[] {
    const widths = rows[0]?.map((_, column) => {
        return Math.max(...rows.map((row) => (row[column] ?? "").length));
    });
    const line = (row: string[]): string => {
        return `| ${row.map((cell, i) => cell.padEnd(widths?.[i] ?? 0)).join(" | ")} |`;
    };
    const separator = (widths ?? []).map((width) => "-".repeat(width));
    return [line(rows[0] ?? []), line(separator), ...rows.slice(1).map(line)];
}

/// One option: a `### section.name` heading, the type/default table, and
/// the description paragraph from the annotation.
function renderField(heading: string, field: FieldSchema): string[] {
    const out: string[] = [];
    out.push(`### \`${heading}\``);
    out.push("");
    out.push(
        ...renderTable([
            ["Type", "Default"],
            [renderType(field), renderDefault(field)],
        ]),
    );
    if (field.description !== undefined) {
        out.push("");
        out.push(field.description);
    }
    return out;
}

/// A section region's body: every option of the section's struct, in
/// declaration order. `rules` is the one array-of-table section; its
/// entries render as `[rules].field`.
function renderSection(root: StructSchema, section: string): string {
    const top = root.properties?.[section];
    if (!top) {
        throw new Error(`schema has no top-level section '${section}'`);
    }
    const isArray = top.type === "array";
    const struct = resolveRef(root, isArray ? (top.items ?? {}) : top);
    const properties = struct.properties ?? {};
    const parts: string[] = [];
    for (const [name, field] of Object.entries(properties)) {
        const heading = isArray ? `[${section}].${name}` : `${section}.${name}`;
        parts.push(renderField(heading, field).join("\n"));
    }
    return parts.join("\n\n");
}

function rewriteDoc(docText: string, root: StructSchema, problems: string[]): string {
    const lines = docText.split("\n");
    const out: string[] = [];
    const seen = new Set<string>();

    let idx = 0;
    while (idx < lines.length) {
        const line = lines[idx] ?? "";
        const match = BEGIN_RE.exec(line);
        if (!match) {
            out.push(line);
            idx += 1;
            continue;
        }
        const section = (match[1] ?? "").trim();
        if (seen.has(section)) {
            problems.push(`${DOC_PATH}: duplicate region '${section}'`);
        }
        seen.add(section);
        let end = idx + 1;
        while (end < lines.length && lines[end] !== END_MARKER) {
            end += 1;
        }
        if (end >= lines.length) {
            problems.push(`${DOC_PATH}: region '${section}' has no closing marker`);
            for (let k = idx; k < lines.length; k++) {
                out.push(lines[k] ?? "");
            }
            return out.join("\n");
        }
        out.push(line);
        out.push("");
        out.push(renderSection(root, section));
        out.push("");
        out.push(lines[end] ?? "");
        idx = end + 1;
    }

    for (const section of Object.keys(root.properties ?? {})) {
        if (!seen.has(section)) {
            problems.push(`${DOC_PATH}: config section '${section}' has no marker region`);
        }
    }
    return out.join("\n");
}

function main(argv: string[]): number {
    const mode = argv[0];
    if (mode !== "update" && mode !== "check") {
        console.error("usage: config_docs.ts update|check");
        return 2;
    }

    const problems: string[] = [];
    const root = JSON.parse(
        fs.readFileSync(path.join(REPO_ROOT, SCHEMA_PATH), "utf8"),
    ) as StructSchema;
    const docPath = path.join(REPO_ROOT, DOC_PATH);
    const current = fs.readFileSync(docPath, "utf8").replaceAll("\r\n", "\n");
    const updated = rewriteDoc(current, root, problems);

    if (problems.length > 0) {
        console.error("config_docs: problems found:");
        for (const problem of problems) {
            console.error(`  - ${problem}`);
        }
        return 1;
    }
    if (current === updated) {
        return 0;
    }
    if (mode === "update") {
        fs.writeFileSync(docPath, updated, "utf8");
        console.log(`updated ${DOC_PATH}`);
        return 0;
    }
    console.error(`config_docs: ${DOC_PATH} is stale; run 'config_docs.ts update'`);
    return 1;
}

process.exit(main(process.argv.slice(2)));
