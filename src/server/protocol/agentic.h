#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kota/ipc/protocol.h"

namespace clice::agentic {

struct CompileCommandParams {
    std::string path;
};

struct CompileCommandResult {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
};

struct FileInfo {
    std::string path;
    std::string kind;
    std::optional<std::string> module_name;
};

struct ProjectFilesParams {
    std::optional<std::string> filter;
};

struct ProjectFilesResult {
    std::vector<FileInfo> files;
    int total = 0;
};

struct DepEntry {
    std::string path;
    int depth = 0;
};

struct FileDepsParams {
    std::string path;
    std::optional<std::string> direction;
    std::optional<int> depth;
};

struct FileDepsResult {
    std::string file;
    std::vector<DepEntry> includes;
    std::vector<DepEntry> includers;
};

struct ImpactAnalysisParams {
    std::string path;
};

struct ImpactAnalysisResult {
    std::vector<std::string> direct_dependents;
    std::vector<std::string> transitive_dependents;
    std::vector<std::string> affected_modules;
};

struct SymbolEntry {
    std::string name;
    std::string kind;
    std::string file;
    int line = 0;
    std::optional<std::string> container;
    std::uint64_t symbol_id = 0;
};

struct SymbolSearchParams {
    std::string query;
    std::optional<std::vector<std::string>> kind_filter;
    std::optional<int> max_results;
};

struct SymbolSearchResult {
    std::vector<SymbolEntry> symbols;
};

struct ReadSymbolParams {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<int> line;
    std::optional<std::uint64_t> symbol_id;
};

struct ReadSymbolResult {
    std::string name;
    std::string kind;
    std::string file;
    int start_line = 0;
    int end_line = 0;
    std::string text;
    std::optional<std::string> signature;
    std::uint64_t symbol_id = 0;
};

struct DocumentSymbolEntry {
    std::string name;
    std::string kind;
    int start_line = 0;
    int end_line = 0;
    std::uint64_t symbol_id = 0;
};

struct DocumentSymbolsParams {
    std::string path;
};

struct DocumentSymbolsResult {
    std::vector<DocumentSymbolEntry> symbols;
};

struct DefinitionParams {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<int> line;
    std::optional<std::uint64_t> symbol_id;
};

struct LocationEntry {
    std::string file;
    int start_line = 0;
    int end_line = 0;
    std::string text;
};

struct DefinitionResult {
    std::string name;
    std::string kind;
    std::uint64_t symbol_id = 0;
    std::optional<LocationEntry> definition;
};

struct ReferenceEntry {
    std::string file;
    int line = 0;
    std::string context;
};

struct ReferencesParams {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<int> line;
    std::optional<std::uint64_t> symbol_id;
    std::optional<bool> include_declaration;
};

struct ReferencesResult {
    std::string name;
    std::string kind;
    std::uint64_t symbol_id = 0;
    std::vector<ReferenceEntry> references;
    int total = 0;
};

struct CallGraphEntry {
    std::string name;
    std::string kind;
    std::string file;
    int line = 0;
    std::uint64_t symbol_id = 0;
};

struct CallGraphParams {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<int> line;
    std::optional<std::uint64_t> symbol_id;
    std::optional<std::string> direction;
    std::optional<int> depth;
};

struct CallGraphResult {
    CallGraphEntry root;
    std::vector<CallGraphEntry> callers;
    std::vector<CallGraphEntry> callees;
};

struct TypeHierarchyEntry {
    std::string name;
    std::string kind;
    std::string file;
    int line = 0;
    std::uint64_t symbol_id = 0;
};

struct TypeHierarchyParams {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<int> line;
    std::optional<std::uint64_t> symbol_id;
    std::optional<std::string> direction;
};

struct TypeHierarchyResult {
    TypeHierarchyEntry root;
    std::vector<TypeHierarchyEntry> supertypes;
    std::vector<TypeHierarchyEntry> subtypes;
};

struct StatusParams {};

struct StatusResult {
    bool idle = true;
    int pending = 0;
    int total = 0;
    int indexed = 0;
};

struct ShutdownParams {};

}  // namespace clice::agentic

namespace kota::ipc::protocol {

template <>
struct RequestTraits<clice::agentic::CompileCommandParams> {
    using Result = clice::agentic::CompileCommandResult;
    constexpr inline static std::string_view method = "agentic/compileCommand";
};

template <>
struct RequestTraits<clice::agentic::ProjectFilesParams> {
    using Result = clice::agentic::ProjectFilesResult;
    constexpr inline static std::string_view method = "agentic/projectFiles";
};

template <>
struct RequestTraits<clice::agentic::FileDepsParams> {
    using Result = clice::agentic::FileDepsResult;
    constexpr inline static std::string_view method = "agentic/fileDeps";
};

template <>
struct RequestTraits<clice::agentic::ImpactAnalysisParams> {
    using Result = clice::agentic::ImpactAnalysisResult;
    constexpr inline static std::string_view method = "agentic/impactAnalysis";
};

template <>
struct RequestTraits<clice::agentic::SymbolSearchParams> {
    using Result = clice::agentic::SymbolSearchResult;
    constexpr inline static std::string_view method = "agentic/symbolSearch";
};

template <>
struct RequestTraits<clice::agentic::ReadSymbolParams> {
    using Result = clice::agentic::ReadSymbolResult;
    constexpr inline static std::string_view method = "agentic/readSymbol";
};

template <>
struct RequestTraits<clice::agentic::DocumentSymbolsParams> {
    using Result = clice::agentic::DocumentSymbolsResult;
    constexpr inline static std::string_view method = "agentic/documentSymbols";
};

template <>
struct RequestTraits<clice::agentic::DefinitionParams> {
    using Result = clice::agentic::DefinitionResult;
    constexpr inline static std::string_view method = "agentic/definition";
};

template <>
struct RequestTraits<clice::agentic::ReferencesParams> {
    using Result = clice::agentic::ReferencesResult;
    constexpr inline static std::string_view method = "agentic/references";
};

template <>
struct RequestTraits<clice::agentic::CallGraphParams> {
    using Result = clice::agentic::CallGraphResult;
    constexpr inline static std::string_view method = "agentic/callGraph";
};

template <>
struct RequestTraits<clice::agentic::TypeHierarchyParams> {
    using Result = clice::agentic::TypeHierarchyResult;
    constexpr inline static std::string_view method = "agentic/typeHierarchy";
};

template <>
struct RequestTraits<clice::agentic::StatusParams> {
    using Result = clice::agentic::StatusResult;
    constexpr inline static std::string_view method = "agentic/status";
};

template <>
struct NotificationTraits<clice::agentic::ShutdownParams> {
    constexpr inline static std::string_view method = "agentic/shutdown";
};

}  // namespace kota::ipc::protocol
