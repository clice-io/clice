/// The product surface the suites must exercise, read from the product
/// itself: the configuration schema, each subcommand's `--help`, and the
/// capabilities and extension methods the server answers.

import { execFileSync } from "node:child_process";
import type * as proto from "vscode-languageserver-protocol";

interface SchemaNode {
    type?: string | string[];
    properties?: Record<string, SchemaNode>;
    items?: SchemaNode;
    $ref?: string;
}

function leaves(
    node: SchemaNode,
    prefix: string,
    defs: Record<string, SchemaNode>,
    out: string[],
): void {
    const resolve = (entry: SchemaNode): SchemaNode =>
        entry.$ref === undefined ? entry : (defs[entry.$ref.split("/").at(-1) ?? ""] ?? entry);
    const resolved = resolve(node);
    const target = resolved.items === undefined ? resolved : resolve(resolved.items);
    if (target.properties === undefined) {
        out.push(prefix);
        return;
    }
    for (const [key, child] of Object.entries(target.properties)) {
        leaves(child, prefix === "" ? key : `${prefix}.${key}`, defs, out);
    }
}

export function configKeys(clice: string): string[] {
    const schema = JSON.parse(
        execFileSync(clice, ["inspect", "config-schema"], { encoding: "utf8" }),
    ) as SchemaNode & {
        $defs?: Record<string, SchemaNode>;
    };
    const out: string[] = [];
    leaves(schema, "", schema.$defs ?? {}, out);
    return out.sort();
}

/// Subcommands and their operations (`query references`, `analyze modules
/// edge`) and their flags, from the help the binary prints.
export function commandsAndFlags(clice: string): { commands: string[]; flags: string[] } {
    const help = (args: string[]): string => {
        try {
            return execFileSync(clice, [...args, "--help"], { encoding: "utf8" });
        } catch (error) {
            return (error as { stdout?: string }).stdout ?? "";
        }
    };
    const commands: string[] = [];
    const flags: string[] = [];
    const visit = (path: string[]): void => {
        const text = help(path);
        const [, listed = ""] = text.split(/^Subcommands:$/m);
        // A subcommand without a description (`worker`) is internal.
        const children = [...listed.matchAll(/^ {2,}([a-z][\w-]*) {2,}\S/gm)].map(
            (match) => match[1] ?? "",
        );
        // A subcommand that runs without an operation prints its own usage
        // above its operations.
        if (path.length > 0 && (children.length === 0 || /^usage: /m.test(text))) {
            commands.push(path.join(" "));
        }
        for (const match of text.matchAll(/^ {2,}(?:-\w\|)?(--[a-z][\w-]*)/gm)) {
            if (match[1] !== "--help") {
                flags.push(`${path.join(" ")} ${match[1] ?? ""}`);
            }
        }
        for (const child of children) {
            visit([...path, child]);
        }
    };
    visit([]);
    return { commands: commands.sort(), flags: [...new Set(flags)].sort() };
}

const CAPABILITY_METHODS: Record<string, string[]> = {
    hoverProvider: ["textDocument/hover"],
    completionProvider: ["textDocument/completion"],
    signatureHelpProvider: ["textDocument/signatureHelp"],
    declarationProvider: ["textDocument/declaration"],
    definitionProvider: ["textDocument/definition"],
    typeDefinitionProvider: ["textDocument/typeDefinition"],
    implementationProvider: ["textDocument/implementation"],
    referencesProvider: ["textDocument/references"],
    documentHighlightProvider: ["textDocument/documentHighlight"],
    documentSymbolProvider: ["textDocument/documentSymbol"],
    codeActionProvider: ["textDocument/codeAction"],
    documentLinkProvider: ["textDocument/documentLink"],
    documentFormattingProvider: ["textDocument/formatting"],
    documentRangeFormattingProvider: ["textDocument/rangeFormatting"],
    renameProvider: ["textDocument/rename", "textDocument/prepareRename"],
    foldingRangeProvider: ["textDocument/foldingRange"],
    selectionRangeProvider: ["textDocument/selectionRange"],
    callHierarchyProvider: [
        "textDocument/prepareCallHierarchy",
        "callHierarchy/incomingCalls",
        "callHierarchy/outgoingCalls",
    ],
    typeHierarchyProvider: [
        "textDocument/prepareTypeHierarchy",
        "typeHierarchy/supertypes",
        "typeHierarchy/subtypes",
    ],
    semanticTokensProvider: ["textDocument/semanticTokens/full"],
    inlayHintProvider: ["textDocument/inlayHint"],
    workspaceSymbolProvider: ["workspace/symbol"],
    diagnosticProvider: ["textDocument/diagnostic"],
    textDocumentSync: [
        "textDocument/didOpen",
        "textDocument/didChange",
        "textDocument/didClose",
        "textDocument/didSave",
    ],
};

/// The methods the advertised capabilities imply, and the clice extension
/// requests a client may send.
export function methods(
    capabilities: proto.ServerCapabilities,
    extensions: readonly string[],
): string[] {
    const out = ["initialize", "initialized", "shutdown", "exit", ...extensions];
    for (const [capability, implied] of Object.entries(CAPABILITY_METHODS)) {
        if ((capabilities as Record<string, unknown>)[capability] !== undefined) {
            out.push(...implied);
        }
    }
    if (capabilities.workspace?.workspaceFolders?.changeNotifications !== undefined) {
        out.push("workspace/didChangeWorkspaceFolders");
    }
    return [...new Set(out)].sort();
}
