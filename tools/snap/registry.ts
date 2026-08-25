/// The single feature registry of the snap domain: both drivers resolve a
/// corpus through it, so a corpus either runs on every path its fixtures
/// ask for or fails loudly — never a silently skipped feature.

import type { AnnotatedSource } from "./annotation.ts";
import { codeCompletion } from "./features/code_completion.ts";
import { documentLinks } from "./features/document_links.ts";
import { documentSymbol } from "./features/document_symbol.ts";
import { foldingRange } from "./features/folding_range.ts";
import { hover } from "./features/hover.ts";
import { inlayHint } from "./features/inlay_hint.ts";
import { navigation } from "./features/navigation.ts";
import { semanticTokens } from "./features/semantic_tokens.ts";
import { signatureHelp } from "./features/signature_help.ts";
import { tuIndex } from "./features/tu_index.ts";
import { workspaceSymbol } from "./features/workspace_symbol.ts";
import type { Feature, FeatureShape } from "./render.ts";

const FEATURES: Record<string, Feature> = {
    code_completion: codeCompletion,
    document_links: documentLinks,
    document_symbol: documentSymbol,
    folding_range: foldingRange,
    hover,
    inlay_hint: inlayHint,
    navigation,
    semantic_tokens: semanticTokens,
    signature_help: signatureHelp,
    tu_index: tuIndex,
    workspace_symbol: workspaceSymbol,
};

export function feature(name: string): Feature {
    const entry = FEATURES[name];
    if (!entry) {
        throw new Error(`no feature registered for tests/snap/${name}`);
    }
    return entry;
}

/// Whether a file of a multi-file fixture is inspected — the TS twin of
/// participates() in src/driver/inspect.cc. Point-driven shapes run purely
/// where their markers are; whole-document shapes always include the unit
/// entry, and a marker opts a support file in.
export function participates(
    shape: FeatureShape,
    source: AnnotatedSource,
    entry: boolean,
): boolean {
    const hasPoints = source.offsets.size > 0 || source.namelessOffsets.length > 0;
    if (shape === "point" || shape === "completion") {
        return hasPoints;
    }
    return entry || hasPoints || source.ranges.size > 0;
}
