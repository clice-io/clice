module;

#include "kota/async/async.h"
#include "kota/async/io/system.h"
#include "kota/codec/bincode/bincode.h"
#include "kota/codec/json/json.h"
#include "kota/codec/toml/toml.h"
#include "kota/codec/visit/common.h"
#include "kota/deco/deco.h"
#include "kota/deco/option.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/progress.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/text.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/protocol.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "kota/meta/annotation.h"
#include "kota/meta/enum.h"
#include "kota/meta/struct.h"
#include "kota/support/ranges.h"
#include "kota/support/type_traits.h"

export module kota;

export namespace kota {

using ::kota::Formattable;
using ::kota::cancellation_source;
using ::kota::cancellation_token;
using ::kota::error;
using ::kota::event;
using ::kota::event_loop;
using ::kota::map_range;
using ::kota::mutex;
using ::kota::outcome_error;
using ::kota::pipe;
using ::kota::process;
using ::kota::queue;
using ::kota::semaphore;
using ::kota::sequence_range;
using ::kota::set_range;
using ::kota::sleep;
using ::kota::small_vector;
using ::kota::task;
using ::kota::task_group;
using ::kota::tcp;
using ::kota::timer;
using ::kota::when_all;
using ::kota::when_any;
using ::kota::with_token;
using ::kota::yield;

}  // namespace kota

export namespace kota::codec {

using ::kota::codec::RawValue;
using ::kota::codec::rich_error;

}  // namespace kota::codec

export namespace kota::codec::bincode {

using ::kota::codec::bincode::from_bytes;
using ::kota::codec::bincode::to_bytes;

}  // namespace kota::codec::bincode

export namespace kota::codec::json {

using ::kota::codec::json::from_json;
using ::kota::codec::json::parse;
using ::kota::codec::json::prettify;
using ::kota::codec::json::to_json;
using ::kota::codec::json::to_string;

}  // namespace kota::codec::json

export namespace kota::codec::toml {

using ::kota::codec::toml::from_toml;
using ::kota::codec::toml::parse;

}  // namespace kota::codec::toml

export namespace kota::deco::cli {

using ::kota::deco::cli::SubCommandError;
using ::kota::deco::cli::SubCommander;
using ::kota::deco::cli::command;
using ::kota::deco::cli::parse;
using ::kota::deco::cli::write_usage_for;

}  // namespace kota::deco::cli

// The deco declaration types the DecoFlag/DecoKV/... macros name in their
// expansions. The macros are force-included textually (they cannot cross a
// module boundary); these declarations reach their users through import kota.
export namespace kota::deco::decl {

using ::kota::deco::decl::CommaJoinedAliasFields;
using ::kota::deco::decl::CommaJoinedFields;
using ::kota::deco::decl::ConfigFields;
using ::kota::deco::decl::FlagAliasFields;
using ::kota::deco::decl::FlagFields;
using ::kota::deco::decl::FlagOption;
using ::kota::deco::decl::InputFields;
using ::kota::deco::decl::InputOption;
using ::kota::deco::decl::KVAliasFields;
using ::kota::deco::decl::KVFields;
using ::kota::deco::decl::KVStyle;
using ::kota::deco::decl::MultiAliasFields;
using ::kota::deco::decl::MultiFields;
using ::kota::deco::decl::OptionCallbackField;
using ::kota::deco::decl::PackFields;
using ::kota::deco::decl::ScalarOption;
using ::kota::deco::decl::VectorOption;

}  // namespace kota::deco::decl

export namespace kota::deco::trait {

using ::kota::deco::trait::InputResultType;
using ::kota::deco::trait::ScalarResultType;
using ::kota::deco::trait::VectorResultType;

}  // namespace kota::deco::trait

export namespace kota::deco::util {

using ::kota::deco::util::argvify;

}  // namespace kota::deco::util

export namespace kota::ipc {

using ::kota::ipc::BincodePeer;
using ::kota::ipc::JsonPeer;
using ::kota::ipc::lsp_config;
using ::kota::ipc::Error;
using ::kota::ipc::RecordingTransport;
using ::kota::ipc::RequestResult;
using ::kota::ipc::StreamTransport;
using ::kota::ipc::Transport;
using ::kota::ipc::request_options;

}  // namespace kota::ipc

export namespace kota::ipc::lsp {

using ::kota::ipc::lsp::LineMap;
using ::kota::ipc::lsp::PositionEncoding;
using ::kota::ipc::lsp::ProgressReporter;
using ::kota::ipc::lsp::URI;
using ::kota::ipc::lsp::build_line_starts;
using ::kota::ipc::lsp::encoded_length;

}  // namespace kota::ipc::lsp

export namespace kota::ipc::protocol {

using ::kota::ipc::protocol::CallHierarchyIncomingCall;
using ::kota::ipc::protocol::CallHierarchyIncomingCallsParams;
using ::kota::ipc::protocol::CallHierarchyItem;
using ::kota::ipc::protocol::CallHierarchyOutgoingCall;
using ::kota::ipc::protocol::CallHierarchyOutgoingCallsParams;
using ::kota::ipc::protocol::CallHierarchyPrepareParams;
using ::kota::ipc::protocol::CodeActionParams;
using ::kota::ipc::protocol::CodeDescription;
using ::kota::ipc::protocol::CompletionItem;
using ::kota::ipc::protocol::CompletionItemKind;
using ::kota::ipc::protocol::CompletionItemLabelDetails;
using ::kota::ipc::protocol::CompletionItemTag;
using ::kota::ipc::protocol::CompletionOptions;
using ::kota::ipc::protocol::CompletionParams;
using ::kota::ipc::protocol::DeclarationOptions;
using ::kota::ipc::protocol::DeclarationParams;
using ::kota::ipc::protocol::DefinitionOptions;
using ::kota::ipc::protocol::DefinitionParams;
using ::kota::ipc::protocol::Diagnostic;
using ::kota::ipc::protocol::DiagnosticRelatedInformation;
using ::kota::ipc::protocol::DiagnosticSeverity;
using ::kota::ipc::protocol::DiagnosticTag;
using ::kota::ipc::protocol::DidChangeTextDocumentParams;
using ::kota::ipc::protocol::DidCloseTextDocumentParams;
using ::kota::ipc::protocol::DidOpenTextDocumentParams;
using ::kota::ipc::protocol::DidSaveTextDocumentParams;
using ::kota::ipc::protocol::DocumentFormattingParams;
using ::kota::ipc::protocol::DocumentLink;
using ::kota::ipc::protocol::DocumentLinkOptions;
using ::kota::ipc::protocol::DocumentLinkParams;
using ::kota::ipc::protocol::DocumentRangeFormattingParams;
using ::kota::ipc::protocol::DocumentSymbol;
using ::kota::ipc::protocol::DocumentSymbolParams;
using ::kota::ipc::protocol::Error;
using ::kota::ipc::protocol::ErrorCode;
using ::kota::ipc::protocol::ExitParams;
using ::kota::ipc::protocol::FoldingRange;
using ::kota::ipc::protocol::FoldingRangeKind;
using ::kota::ipc::protocol::FoldingRangeParams;
using ::kota::ipc::protocol::Hover;
using ::kota::ipc::protocol::HoverParams;
using ::kota::ipc::protocol::ImplementationOptions;
using ::kota::ipc::protocol::ImplementationParams;
using ::kota::ipc::protocol::InitializeParams;
using ::kota::ipc::protocol::InitializeResult;
using ::kota::ipc::protocol::InitializedParams;
using ::kota::ipc::protocol::InlayHint;
using ::kota::ipc::protocol::InlayHintKind;
using ::kota::ipc::protocol::InlayHintLabelPart;
using ::kota::ipc::protocol::InlayHintParams;
using ::kota::ipc::protocol::InsertTextFormat;
using ::kota::ipc::protocol::LSPAny;
using ::kota::ipc::protocol::LSPVariant;
using ::kota::ipc::protocol::Location;
using ::kota::ipc::protocol::LogMessageParams;
using ::kota::ipc::protocol::MarkupContent;
using ::kota::ipc::protocol::MarkupKind;
using ::kota::ipc::protocol::MessageType;
using ::kota::ipc::protocol::NotificationTraits;
using ::kota::ipc::protocol::ParameterInformation;
using ::kota::ipc::protocol::Position;
using ::kota::ipc::protocol::ProgressToken;
using ::kota::ipc::protocol::PublishDiagnosticsParams;
using ::kota::ipc::protocol::Range;
using ::kota::ipc::protocol::ReferenceOptions;
using ::kota::ipc::protocol::ReferenceParams;
using ::kota::ipc::protocol::RequestTraits;
using ::kota::ipc::protocol::SaveOptions;
using ::kota::ipc::protocol::SemanticTokens;
using ::kota::ipc::protocol::SemanticTokensLegend;
using ::kota::ipc::protocol::SemanticTokensOptions;
using ::kota::ipc::protocol::SemanticTokensParams;
using ::kota::ipc::protocol::ServerInfo;
using ::kota::ipc::protocol::ShutdownParams;
using ::kota::ipc::protocol::SignatureHelp;
using ::kota::ipc::protocol::SignatureHelpOptions;
using ::kota::ipc::protocol::SignatureHelpParams;
using ::kota::ipc::protocol::SymbolInformation;
using ::kota::ipc::protocol::SymbolKind;
using ::kota::ipc::protocol::TextDocumentContentChangeEvent;
using ::kota::ipc::protocol::TextDocumentContentChangePartial;
using ::kota::ipc::protocol::TextDocumentContentChangeWholeDocument;
using ::kota::ipc::protocol::TextDocumentSyncKind;
using ::kota::ipc::protocol::TextDocumentSyncOptions;
using ::kota::ipc::protocol::TextEdit;
using ::kota::ipc::protocol::TypeDefinitionOptions;
using ::kota::ipc::protocol::TypeDefinitionParams;
using ::kota::ipc::protocol::TypeHierarchyItem;
using ::kota::ipc::protocol::TypeHierarchyPrepareParams;
using ::kota::ipc::protocol::TypeHierarchySubtypesParams;
using ::kota::ipc::protocol::TypeHierarchySupertypesParams;
using ::kota::ipc::protocol::Value;
using ::kota::ipc::protocol::WorkspaceSymbolParams;
using ::kota::ipc::protocol::boolean;
using ::kota::ipc::protocol::integer;
using ::kota::ipc::protocol::nullable;
using ::kota::ipc::protocol::string;
using ::kota::ipc::protocol::uinteger;
using ::kota::ipc::protocol::variant;

}  // namespace kota::ipc::protocol

export namespace kota::meta {

using ::kota::meta::annotation;
using ::kota::meta::defaulted;
using ::kota::meta::enum_name;
using ::kota::meta::enum_type;
using ::kota::meta::for_each;
using ::kota::meta::reflectable_class;
using ::kota::meta::reflection;

}  // namespace kota::meta

export namespace kota::meta::attrs {

using ::kota::meta::attrs::skip;

}

export namespace kota::option {

using ::kota::option::OptTable;
using ::kota::option::ParseOptions;
using ::kota::option::ParsedArg;

}  // namespace kota::option

export namespace kota::sys {

using ::kota::sys::memory;
using ::kota::sys::parallelism;
using ::kota::sys::priority;
using ::kota::sys::set_priority;

}  // namespace kota::sys
