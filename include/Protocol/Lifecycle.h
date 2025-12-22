#pragma once

#include "Basic.h"
#include "Notebook.h"
#include "TextDocument.h"
#include "Workspace.h"

/// clice currently ignores all `dynamicRegistration` field in LSP specification.

namespace clice::proto {

struct LSPInfo {
    /// The name of server or client.
    std::string name;

    /// The version of server or client.
    std::string version;
};

struct WindowCapacities {};

struct RegularExpressionsClientCapabilities {};

struct MarkdownClientCapabilities {};

struct GeneralCapacities {
    /// FIXME: staleRequestSupport

    /// Client capabilities specific to regular expressions.
    optional<RegularExpressionsClientCapabilities> regularExpressions;

    /// Client capabilities specific to the client's markdown parser.
    optional<MarkdownClientCapabilities> markdown;

    /// The position encodings supported by the client.
    optional<array<PositionEncodingKind>> positionEncodings;
};

struct ClientCapabilities {
    /// Workspace specific client capabilities.
    WorkspaceClientCapabilities workspace;

    /// Text document specific client capabilities.
    TextDocumentClientCapabilities textDocument;

    /// Capabilities specific to the notebook document support.
    NotebookDocumentClientCapabilities notebookDocument;

    /// Window specific client capabilities.
    WindowCapacities window;

    /// General client capabilities.
    GeneralCapacities general;
};

struct InitializeParams {
    /// Information about client.
    LSPInfo clientInfo;

    /// The capabilities provided by the client (editor or tool).
    ClientCapabilities capabilities;

    /// The workspace folders configured in the client when the server starts.
    /// This property is only available if the client supports workspace folders.
    /// It can be `null` if the client supports workspace folders but none are
    /// configured.
    optional<array<WorkspaceFolder>> workspaceFolders;

    /// The rootUri of the workspace. Is null if no
    /// folder is open. If both `rootPath` and `rootUri` are set
    /// `rootUri` wins.
    ///
    /// Deprecated in favour of `workspaceFolders`
    optional<URI> rootUri;
};

struct ServerCapabilities {
    /// The position encoding the server picked from the encodings offered
    /// by the client via the client capability `general.positionEncodings`.
    PositionEncodingKind positionEncoding;

    /// Defines how text documents are synced.
    TextDocumentSyncOptions textDocumentSync;

    /// Defines how notebook documents are synced.
    /// FIXME: NotebookDocumentSyncOptions notebookDocumentSync;

    /// The server provides completion support.
    CompletionOptions completionProvider;

    /// The server provides hover support.
    HoverOptions hoverProvider;

    /// The server provides signature help support.
    SignatureHelpOptions signatureHelpProvider;

    /// The server provides go to declaration support.
    DeclarationOptions declarationProvider;

    /// The server provides goto definition support.
    DefinitionOptions definitionProvider;

    /// The server provides goto type definition support.
    /// FIXME: TypeDefinitionOptions typeDefinitionProvider;

    /// The server provides goto implementation support.
    /// FIXME: ImplementationOptions implementationProvider;

    /// The server provides find references support.
    ReferenceOptions referencesProvider;

    /// The server provides document highlight support.
    /// FIXME: DocumentHighlightOptions documentHighlightProvider;

    /// The server provides document symbol support.
    DocumentSymbolOptions documentSymbolProvider;

    /// The server provides code actions. The `CodeActionOptions` return type is
    /// only valid if the client signals code action literal support via the
    /// property `textDocument.codeAction.codeActionLiteralSupport`.
    /// FIXME: CodeActionOptions codeActionProvider;

    /// The server provides code lens.
    /// FIXME: CodeLensOptions codeLensProvider;

    /// The server provides document link support.
    DocumentLinkOptions documentLinkProvider;

    /// The server provides color provider support.
    /// FIXME: DocumentColorOptions colorProvider;

    /// The server provides document formatting.
    DocumentFormattingOptions documentFormattingProvider;

    /// The server provides document range formatting.
    DocumentRangeFormattingOptions documentRangeFormattingProvider;

    /// The server provides document formatting on typing.
    /// FIXME: DocumentOnTypeFormattingOptions documentOnTypeFormattingProvider;

    /// The server provides rename support. RenameOptions may only be specified if the client
    /// states that it supports `prepareSupport` in its initial `initialize` request.
    /// FIXME: RenameOptions renameProvider;

    /// The server provides folding provider support.
    FoldingRangeOptions foldingRangeProvider;

    /// The server provides execute command support.
    /// FIXME: ExecuteCommandOptions executeCommandProvider;

    /// The server provides selection range support.
    /// FIXME: SelectionRangeOptions selectionRangeProvider;

    /// The server provides linked editing range support.
    /// FIXME: LinkedEditingRangeOptions linkedEditingRangeProvider;

    /// The server provides call hierarchy support.
    /// FIXME: CallHierarchyOptions callHierarchyProvider;

    /// The server provides semantic tokens support.
    SemanticTokensOptions semanticTokensProvider;

    /// Whether server provides moniker support.
    /// FIXME: MonikerOptions monikerProvider;

    /// The server provides type hierarchy support.
    /// FIXME: TypeHierarchyOptions typeHierarchyProvider;

    /// The server provides inline values.
    /// FIXME: InlineValueOptions inlineValueProvider;

    /// The server provides inlay hints.
    InlayHintOptions inlayHintProvider;

    /// The server has support for pull model diagnostics.
    /// FIXME: DiagnosticOptions diagnosticProvider;

    /// The server provides workspace symbol support.
    WorkspaceSymbolOptions workspaceSymbolProvider;

    /// Workspace specific server capabilities.
    WorkspaceServerCapabilities workspace;
};

struct InitializeResult {
    /// Information about the server.
    LSPInfo serverInfo;

    /// The capabilities the language server provides.
    ServerCapabilities capabilities;
};

struct Empty {};

using InitializedParams = Empty;

using ShutdownParams = Empty;

using ShutdownResult = Empty;

using ExitParams = Empty;

using ExitResult = Empty;

}  // namespace clice::proto
