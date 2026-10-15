/// The directories of tests/: its topics (tests/README.md, Topics)
/// and the suites beside them. Any other directory there is an error.

/// Each topic's family: what a client answers about code, how the server
/// works, or a command of the CLI.
export const TOPICS: Readonly<Record<string, "feature" | "engine" | "command">> = {
    hover: "feature",
    completion: "feature",
    signature_help: "feature",
    navigation: "feature",
    document_highlight: "feature",
    document_symbol: "feature",
    workspace_symbol: "feature",
    document_links: "feature",
    folding_range: "feature",
    selection_range: "feature",
    semantic_tokens: "feature",
    inlay_hints: "feature",
    code_actions: "feature",
    switch_source_header: "feature",
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
    scan: "command",
    cli: "command",
};

/// Suites that are not cases.
export const SUITES = new Set(["editors", "nightly", "compat", "node_modules"]);
