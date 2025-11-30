#pragma once

#include "Basic.h"
#include "Feature/CallHierarchy.h"
#include "Feature/CodeAction.h"
#include "Feature/CodeCompletion.h"
#include "Feature/CodeLens.h"
#include "Feature/Declaration.h"
#include "Feature/Definition.h"
#include "Feature/Diagnostic.h"
#include "Feature/DocumentHighlight.h"
#include "Feature/DocumentLink.h"
#include "Feature/DocumentSymbol.h"
#include "Feature/FoldingRange.h"
#include "Feature/Formatting.h"
#include "Feature/Hover.h"
#include "Feature/Implementation.h"
#include "Feature/InlayHint.h"
#include "Feature/Reference.h"
#include "Feature/Rename.h"
#include "Feature/SemanticTokens.h"
#include "Feature/SignatureHelp.h"
#include "Feature/TypeDefinition.h"
#include "Feature/TypeHierarchy.h"

namespace clice::proto {

struct TextDocumentSyncClientCapabilities {};

struct TextDocumentClientCapabilities {
    optional<TextDocumentSyncClientCapabilities> synchronization;

    /// Capabilities specific to the `textDocument/completion` request.
    optional<CompletionClientCapabilities> completion;

    /// Capabilities specific to the `textDocument/hover` request.
    optional<HoverClientCapabilities> hover;

    /// Capabilities specific to the `textDocument/signatureHelp` request.
    optional<SignatureHelpClientCapabilities> signatureHelp;

    /// Capabilities specific to the `textDocument/declaration` request.
    optional<DeclarationClientCapabilities> declaration;

    /// Capabilities specific to the `textDocument/definition` request.
    optional<DefinitionClientCapabilities> definition;

    /// Capabilities specific to the `textDocument/typeDefinition` request.
    optional<TypeDefinitionClientCapabilities> typeDefinition;

    /// Capabilities specific to the `textDocument/implementation` request.
    optional<ImplementationClientCapabilities> implementation;

    /// Capabilities specific to the `textDocument/references` request.
    optional<ReferenceClientCapabilities> references;

    /// Capabilities specific to the `textDocument/documentHighlight` request.
    optional<DocumentHighlightClientCapabilities> documentHighlight;

    /// Capabilities specific to the `textDocument/documentSymbol` request.
    optional<DocumentSymbolClientCapabilities> documentSymbol;

    /// Capabilities specific to the `textDocument/codeAction` request.
    optional<CodeActionClientCapabilities> codeAction;

    /// Capabilities specific to the `textDocument/codeLens` request.
    optional<CodeLensClientCapabilities> codeLens;

    /// Capabilities specific to the `textDocument/documentLink` request.
    optional<DocumentLinkClientCapabilities> documentLink;

    /// Capabilities specific to the `textDocument/documentColor` and the
    /// `textDocument/colorPresentation` request.
    /// FIXME: optional<DocumentColorClientCapabilities> colorProvider;

    /// Capabilities specific to the `textDocument/formatting` request.
    optional<DocumentFormattingClientCapabilities> formatting;

    /// Capabilities specific to the `textDocument/rangeFormatting` request.
    optional<DocumentRangeFormattingClientCapabilities> rangeFormatting;

    /// Capabilities specific to the `textDocument/onTypeFormatting` request.
    optional<DocumentOnTypeFormattingClientCapabilities> onTypeFormatting;

    /// Capabilities specific to the `textDocument/rename` request.
    optional<RenameClientCapabilities> rename;

    /// Capabilities specific to the `textDocument/publishDiagnostics` notification.
    optional<PublishDiagnosticsClientCapabilities> publishDiagnostics;

    /// Capabilities specific to the `textDocument/foldingRange` request.
    optional<FoldingRangeClientCapabilities> foldingRange;

    /// Capabilities specific to the `textDocument/selectionRange` request.
    /// FIXME: optional<SelectionRangeClientCapabilities> selectionRange;

    /// Capabilities specific to the `textDocument/linkedEditingRange` request.
    /// FIXME: optional<LinkedEditingRangeClientCapabilities> linkedEditingRange;

    /// Capabilities specific to the various call hierarchy requests.
    optional<CallHierarchyClientCapabilities> callHierarchy;

    /// Capabilities specific to the various semantic token requests.
    optional<SemanticTokensClientCapabilities> semanticTokens;

    /// Capabilities specific to the `textDocument/moniker` request.
    /// FIXME: optional<MonikerClientCapabilities> moniker;

    /// Capabilities specific to the various type hierarchy requests.
    optional<TypeHierarchyClientCapabilities> typeHierarchy;

    /// Capabilities specific to the `textDocument/inlineValue` request.
    /// FIXME: optional<InlineValueClientCapabilities> inlineValue;

    /// Capabilities specific to the `textDocument/inlayHint` request.
    optional<InlayHintClientCapabilities> inlayHint;

    /// Capabilities specific to the diagnostic pull model.
    optional<DiagnosticClientCapabilities> diagnostic;
};

enum class TextDocumentSyncKind : std::uint8_t {
    /// Documents should not be synced at all.
    None = 0,

    /// Documents are synced by always sending the full content of the document.
    Full = 1,

    /// Documents are synced by sending the full content on open. After that
    /// only incremental updates to the document are sent.
    Incremental = 2,
};

struct TextDocumentSyncOptions {
    /// Open and close notifications are sent to the server. If omitted open
    /// close notifications should not be sent.
    bool openClose = true;

    /// Change notifications are sent to the server.
    TextDocumentSyncKind change = TextDocumentSyncKind::Incremental;

    /// If present will save notifications are sent to the server. If omitted
    /// the notification should not be sent.
    /// FIXME: bool willSave;

    /// If present will save wait until requests are sent to the server. If
    /// omitted the request should not be sent.
    /// FIXME: bool willSaveWaitUntil;

    /// If present save notifications are sent to the server. If omitted the
    /// notification should not be sent.
    bool save = true;
};

struct DidOpenTextDocumentParams {
    /// The document that was opened.
    TextDocumentItem textDocument;
};

struct TextDocumentContentChangeEvent {
    /// The new text of the whole document.
    string text;
};

struct DidChangeTextDocumentParams {
    /// The document that did change. The version number points
    /// to the version after all provided content changes have
    /// been applied.
    VersionedTextDocumentIdentifier textDocument;

    /// The actual content changes. The content changes describe single state
    /// changes to the document. So if there are two content changes c1 (at
    /// array index 0) and c2 (at array index 1) for a document in state S then
    /// c1 moves the document from S to S' and c2 from S' to S''. So c1 is
    /// computed on the state S and c2 is computed on the state S'.
    //
    /// To mirror the content of a document using change events use the following
    /// approach:
    /// - start with the same initial content
    /// - apply the 'textDocument/didChange' notifications in the order you
    ///   receive them.
    /// - apply the `TextDocumentContentChangeEvent`s in a single notification
    ///   in the order you receive them.
    array<TextDocumentContentChangeEvent> contentChanges;
};

struct DidSaveTextDocumentParams {
    /// The document that was saved.
    TextDocumentIdentifier textDocument;

    /// Optional the content when saved. Depends on the includeText value
    /// when the save notification was requested.
    string text;
};

struct DidCloseTextDocumentParams {
    /// The document that was closed.
    TextDocumentIdentifier textDocument;
};

}  // namespace clice::proto
