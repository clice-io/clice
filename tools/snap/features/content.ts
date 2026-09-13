import type { Feature } from "../render.ts";
import { normalizeFilePath, yamlStr } from "../snapshot.ts";

/// The content-hash dump has no LSP request shape, so content fixtures
/// are `verify: inspect` and the server adapter is unreachable.
interface RawContentDep {
    file: number;
    unit: number;
}

interface RawContentUnit {
    line: number;
    end_line: number;
    kind: string;
    name: string;
    entity: string;
    own: string;
    content: string;
    deps: RawContentDep[];
}

interface RawContentFile {
    path: string;
    digest: string;
    units: RawContentUnit[];
}

/// Labels for hash values in order of first appearance: equal hashes get
/// equal labels, so a snapshot pins which units share a hash without
/// pinning the bytes, which depend on the target and the compiler build.
class Classes {
    private readonly labels = new Map<string, number>();

    constructor(private readonly prefix: string) {}

    of(hash: string): string {
        let index = this.labels.get(hash);
        if (index === undefined) {
            index = this.labels.size;
            this.labels.set(hash, index);
        }
        return `${this.prefix}${index}`;
    }
}

export const content: Feature = {
    shape: "document",
    fromInspect(entry, ctx) {
        const files = entry.result as RawContentFile[];
        const label = (dep: RawContentDep): string => {
            const file = files[dep.file];
            const unit = file?.units[dep.unit];
            if (file === undefined || unit === undefined) {
                throw new Error(`content dump references a missing unit ${dep.file}/${dep.unit}`);
            }
            return `${normalizeFilePath(file.path, ctx.root)}:${unit.line}`;
        };
        const digests = new Classes("d");
        const owns = new Classes("o");
        const contents = new Classes("c");
        const out: string[] = [];
        for (const file of files) {
            if (out.length > 0) {
                out.push("");
            }
            out.push(
                `${normalizeFilePath(file.path, ctx.root)}: { digest: ${digests.of(file.digest)} }`,
            );
            for (const unit of file.units) {
                let line =
                    `- { lines: "${unit.line}-${unit.end_line}", kind: ${unit.kind}` +
                    (unit.name === "" ? "" : `, name: ${yamlStr(unit.name)}`) +
                    `, own: ${owns.of(unit.own)}, content: ${contents.of(unit.content)}`;
                if (unit.deps.length > 0) {
                    line += `, deps: [${unit.deps.map(label).join(", ")}]`;
                }
                out.push(line + " }");
            }
        }
        return out;
    },
    fromServer() {
        return Promise.reject(new Error("content has no server path; use verify: inspect"));
    },
};
