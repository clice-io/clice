/// Snapshot assertions for integration tests, mirroring zest's semantics.
///
/// File format, layout and update workflow follow the C++ side
/// (kota::zest snapshot support) so both layers share one muscle memory:
///
///     ---
///     source: snapshots.test.ts
///     created_at: 2026-07-26
///     input_file: fold_kinds/block_folding.cpp
///     ---
///     <body>
///
/// A missing snapshot is created and passes; a mismatch writes a sibling
/// `.snap.yml.new` and fails with a diff; UPDATE_SNAPSHOTS=1 rewrites the
/// stored snapshot preserving its created_at.

import * as fs from "node:fs";
import * as path from "node:path";
import { URI } from "vscode-uri";

export const WORKSPACE_PLACEHOLDER = "${WS}";

// Bytes RFC 3986 does not allow to appear raw in a URI (spaces, control
// characters, non-ASCII, ...). Their presence means the server skipped
// percent-encoding; lenient parsers would silently tolerate them.
const URI_ILLEGAL_CHAR = /[^A-Za-z0-9\-._~:/?#[\]@!$&'()*+,;=%]/;
const URI_BAD_PERCENT = /%(?![0-9A-Fa-f]{2})/;

/// Validate a wire URI and rewrite it to a platform-independent form.
///
/// This is deliberately an invariant check, not a best-effort cleanup: any
/// server response field holding a file reference must be a well-formed
/// `file://` URI whose decoded path exists inside the test workspace.
/// A raw filesystem path fails here on every platform, instead of only
/// breaking real clients on Windows. vscode-uri is used strictly as the
/// decoder; its tolerance for malformed input must never replace the
/// checks above it.
export function normalizeFileUri(uri: string, workspace: string): string {
    const illegal = URI_ILLEGAL_CHAR.exec(uri);
    if (illegal) {
        throw new Error(
            `raw ${JSON.stringify(illegal[0])} in URI (missing percent-encoding): ${uri}`,
        );
    }
    if (URI_BAD_PERCENT.test(uri)) {
        throw new Error(`malformed percent-encoding in URI: ${uri}`);
    }

    // Check the scheme on the raw string: URI.parse in non-strict mode
    // silently upgrades a bare path to a file URI, which is exactly the
    // malformed reply this validator exists to reject.
    // Schemes are case-insensitive (RFC 3986); lowercase like urlsplit did.
    const scheme = (/^([A-Za-z][A-Za-z0-9+.-]*):/.exec(uri)?.[1] ?? "").toLowerCase();
    if (scheme !== "file") {
        throw new Error(`not a file:// URI (scheme=${JSON.stringify(scheme)}): ${uri}`);
    }
    const parsed = URI.parse(uri);
    if (parsed.authority) {
        throw new Error(`unexpected authority in file URI: ${uri}`);
    }
    if (parsed.query || parsed.fragment) {
        throw new Error(`unexpected query/fragment in file URI: ${uri}`);
    }

    // vscode-uri owns the percent-decoding and drive-letter handling.
    const fsPath = parsed.fsPath;
    if (!path.isAbsolute(fsPath)) {
        throw new Error(`URI does not decode to an absolute path: ${uri}`);
    }
    if (!fs.existsSync(fsPath)) {
        throw new Error(`URI target does not exist on disk: ${uri} -> ${fsPath}`);
    }

    const resolved = fs.realpathSync.native(fsPath);
    const ws = fs.realpathSync.native(workspace);
    const rel = path.relative(ws, resolved);
    if (rel.startsWith("..") || path.isAbsolute(rel)) {
        throw new Error(`URI target escapes the workspace ${ws}: ${uri}`);
    }
    return `${WORKSPACE_PLACEHOLDER}/${rel.split(path.sep).join("/")}`;
}

/// Quote a string exactly like tests/unit/test/tester.h yaml_str, so
/// snapshot bodies read identically across the layers.
export function yamlStr(s: string): string {
    let out = '"';
    for (const c of s) {
        if (c === '"') {
            out += '\\"';
        } else if (c === "\\") {
            out += "\\\\";
        } else if (c === "\n") {
            out += "\\n";
        } else if (c === "\r") {
            out += "\\r";
        } else if (c === "\t") {
            out += "\\t";
        } else if (c.charCodeAt(0) < 0x20) {
            out += `\\x${c.charCodeAt(0).toString(16).padStart(2, "0")}`;
        } else {
            out += c;
        }
    }
    return out + '"';
}

/// Value of `- key: value` in a fixture's leading `///` doc header, or "".
/// Mirrors tests/unit/test/fixture.h; feature_docs.ts is the authority on
/// the full grammar.
export function fixtureFrontmatter(content: string, key: string): string {
    let first = (content.split("\n", 1)[0] ?? "").trim();
    if (!first.startsWith("///")) {
        return "";
    }
    first = first.slice(3);
    if (first.startsWith(" ")) {
        first = first.slice(1);
    }
    if (!first.startsWith("# ")) {
        return "";
    }
    for (let line of content.split("\n")) {
        line = line.trim();
        if (!line.startsWith("///")) {
            break;
        }
        line = line.slice(3).trim();
        if (!line.startsWith("- ") || !line.includes(":")) {
            continue;
        }
        const sep = line.indexOf(":");
        if (line.slice(2, sep).trim() === key) {
            return line.slice(sep + 1).trim();
        }
    }
    return "";
}

export interface Snapshot {
    createdAt: string;
    body: string;
}

export function parseSnap(text: string): Snapshot | null {
    const lines = text.replaceAll("\r\n", "\n").split("\n");
    if (lines[0] !== "---") {
        return null;
    }
    let createdAt = "";
    for (let i = 1; i < lines.length; i++) {
        const line = lines[i] ?? "";
        if (line === "---") {
            return { createdAt, body: lines.slice(i + 1).join("\n") };
        }
        if (line.startsWith("created_at:")) {
            createdAt = line.slice("created_at:".length).trim();
        }
    }
    return null;
}

export function formatSnap(
    source: string,
    inputFile: string,
    body: string,
    createdAt = "",
): string {
    if (!createdAt) {
        createdAt = new Date().toISOString().slice(0, 10);
    }
    if (body && !body.endsWith("\n")) {
        body += "\n";
    }
    return `---\nsource: ${source}\ncreated_at: ${createdAt}\ninput_file: ${inputFile}\n---\n${body}`;
}

function diffLines(oldBody: string, newBody: string): string {
    // Line-by-line diff, zest-style: paired -/+ up to the longer side.
    const oldLines = oldBody.split("\n");
    const newLines = newBody.split("\n");
    const max = Math.max(oldLines.length, newLines.length);
    const out: string[] = [];
    for (let i = 0; i < max && out.length < 40; i++) {
        if (oldLines[i] === newLines[i]) {
            continue;
        }
        if (i < oldLines.length) {
            out.push(`-  ${oldLines[i]}`);
        }
        if (i < newLines.length) {
            out.push(`+  ${newLines[i]}`);
        }
    }
    return out.join("\n");
}

/// One feature's snapshot directory plus the run-wide update flag.
export class SnapshotContext {
    constructor(
        readonly directory: string,
        readonly update: boolean = process.env["UPDATE_SNAPSHOTS"] === "1",
        readonly source = "snapshots.test.ts",
    ) {}

    check(inputFile: string, body: string): void {
        body = body.replaceAll("\r\n", "\n");
        if (body && !body.endsWith("\n")) {
            body += "\n";
        }

        const snapPath = path.join(this.directory, `${inputFile}.snap.yml`);
        const newPath = `${snapPath}.new`;

        const existing = fs.existsSync(snapPath)
            ? parseSnap(fs.readFileSync(snapPath, "utf8"))
            : null;
        if (existing === null) {
            fs.mkdirSync(path.dirname(snapPath), { recursive: true });
            fs.writeFileSync(snapPath, formatSnap(this.source, inputFile, body));
            console.log(`[snapshot] created ${snapPath}`);
            return;
        }

        if (existing.body === body) {
            fs.rmSync(newPath, { force: true });
            return;
        }

        if (this.update) {
            fs.writeFileSync(
                snapPath,
                formatSnap(this.source, inputFile, body, existing.createdAt),
            );
            fs.rmSync(newPath, { force: true });
            console.log(`[snapshot] updated ${snapPath}`);
            return;
        }

        fs.writeFileSync(newPath, formatSnap(this.source, inputFile, body));
        throw new Error(
            `snapshot mismatch: ${snapPath}\n` +
                `new result written to ${newPath}\n` +
                `${diffLines(existing.body, body)}\n` +
                "run with UPDATE_SNAPSHOTS=1 to accept",
        );
    }
}
