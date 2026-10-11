/// File references in answers, checked and made workspace-relative.
///
/// A file URI the server sends is an invariant, not a best-effort input:
/// it must be a well-formed `file://` URI whose decoded path exists, and a
/// raw path or an unencoded space fails here on every platform instead of
/// breaking real clients on Windows. vscode-uri decodes; it never stands in
/// for the checks.

import * as fs from "node:fs";
import * as path from "node:path";
import { URI } from "vscode-uri";
import { cliceExecutable } from "../../client/session.ts";

/// A directory outside the workspace answers may point into, by every path
/// that reaches it, and the name its files render under
/// (`${RESOURCE}/include/stddef.h`).
export interface Root {
    dirs: string[];
    name: string;
}

// Bytes RFC 3986 does not allow raw in a URI (spaces, control characters,
// non-ASCII): their presence means the server skipped percent-encoding.
const URI_ILLEGAL_CHAR = /[^A-Za-z0-9\-._~:/?#[\]@!$&'()*+,;=%]/;
const URI_BAD_PERCENT = /%(?![0-9A-Fa-f]{2})/;

/// The decoded absolute path of a URI as the protocol carries it.
export function uriToPath(uri: string): string {
    const illegal = URI_ILLEGAL_CHAR.exec(uri);
    if (illegal) {
        throw new Error(
            `raw ${JSON.stringify(illegal[0])} in URI (missing percent-encoding): ${uri}`,
        );
    }
    if (URI_BAD_PERCENT.test(uri)) {
        throw new Error(`malformed percent-encoding in URI: ${uri}`);
    }
    // The scheme is checked on the raw text: URI.parse quietly upgrades a
    // bare path to a file URI, which is the malformed reply to catch.
    const scheme = (/^([A-Za-z][A-Za-z0-9+.-]*):/.exec(uri)?.[1] ?? "").toLowerCase();
    if (scheme !== "file") {
        throw new Error(`not a file:// URI (scheme ${JSON.stringify(scheme)}): ${uri}`);
    }
    const parsed = URI.parse(uri);
    if (parsed.authority !== "" || parsed.query !== "" || parsed.fragment !== "") {
        throw new Error(`file URI with an authority, query or fragment: ${uri}`);
    }
    if (!path.isAbsolute(parsed.fsPath)) {
        throw new Error(`URI does not decode to an absolute path: ${uri}`);
    }
    return parsed.fsPath;
}

let resource: Root | undefined;

/// clang's resource directory beside the clice under test, `${RESOURCE}`:
/// the builtin headers (`stddef.h`, `stdint.h`) every compile finds.
export function resourceRoot(): Root {
    if (resource === undefined) {
        // clice finds it from its own resolved path; a build links each
        // header to where the toolchain unpacked it.
        const exe = fs.realpathSync.native(cliceExecutable());
        const lib = path.join(path.dirname(exe), "..", "lib", "clang");
        const [version] = fs.readdirSync(lib);
        if (version === undefined) {
            throw new Error(`no clang resource directory in ${lib}`);
        }
        const dir = path.join(lib, version);
        const header = fs.realpathSync.native(path.join(dir, "include", "stddef.h"));
        resource = {
            dirs: [dir, path.dirname(path.dirname(header))],
            name: "${RESOURCE}",
        };
    }
    return resource;
}

/// The name a file renders under: workspace-relative POSIX, under the name
/// of the root holding it, or else its absolute path. A file only an open
/// buffer holds is named as it is; reading it is what proves it.
export function displayPath(file: string, workspace: string, roots: readonly Root[] = []): string {
    const resolved = fs.existsSync(file) ? fs.realpathSync.native(file) : file;
    for (const root of [{ dirs: [fs.realpathSync.native(workspace)], name: "" }, ...roots]) {
        for (const dir of root.dirs) {
            const relative = path.relative(dir, root.name === "" ? resolved : file);
            if (!relative.startsWith("..") && !path.isAbsolute(relative)) {
                const posix = relative.split(path.sep).join("/");
                return root.name === "" ? posix : `${root.name}/${posix}`;
            }
        }
    }
    return resolved.split(path.sep).join("/");
}

/// The file a name outside the workspace stands for, or undefined for a
/// workspace file.
export function outsidePath(name: string, roots: readonly Root[]): string | undefined {
    for (const root of roots) {
        const [dir] = root.dirs;
        if (dir !== undefined && name.startsWith(`${root.name}/`)) {
            return path.join(dir, name.slice(root.name.length + 1));
        }
    }
    return path.isAbsolute(name) ? name : undefined;
}

export function displayUri(uri: string, workspace: string, roots: readonly Root[] = []): string {
    return displayPath(uriToPath(uri), workspace, roots);
}
