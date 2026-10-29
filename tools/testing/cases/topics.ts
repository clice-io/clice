/// The directories of tests/: its topics (tests/README.md, Topics)
/// and the suites beside them. Any other directory there is an error.

/// Each topic's family: what a client answers about code, how the server
/// works, or a command of the CLI.
export const TOPICS: Readonly<Record<string, "feature" | "engine" | "command">> = {
    hover: "feature",
    code_completion: "feature",
    signature_help: "feature",
    navigation: "feature",
    document_highlights: "feature",
    document_symbols: "feature",
    workspace_symbols: "feature",
    document_links: "feature",
    folding_ranges: "feature",
    selection_ranges: "feature",
    semantic_tokens: "feature",
    inlay_hints: "feature",
    code_actions: "feature",
    counterparts: "feature",
    rename: "feature",
    diagnostics: "feature",
    inactive_regions: "feature",
    compile: "engine",
    contexts: "engine",
    project: "engine",
    modules: "engine",
    config: "engine",
    protocol: "engine",
    lifecycle: "engine",
    index: "command",
    query: "command",
    refactor: "command",
    format: "command",
    lint: "command",
    analyze: "command",
    modularize: "command",
    inspect: "command",
    command: "command",
    scan: "command",
    cli: "command",
};

/// Suites that are not cases.
export const SUITES = new Set(["editors", "compat", "node_modules"]);
