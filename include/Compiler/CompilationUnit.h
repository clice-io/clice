#pragma once

#include "Directive.h"
#include "Compiler/Diagnostic.h"
#include "AST/SymbolID.h"
#include "AST/SourceCode.h"
#include "AST/Resolver.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice {

/// All AST related information needed for language server.
class CompilationUnit {
public:
    /// The kind describes how we preprocess this source file
    /// to get this compilation unit.
    enum class Kind : std::uint8_t {
        /// From preprocessing the source file. Therefore directives
        /// are available but AST nodes are not.
        Preprocess,

        /// From indexing the static source file.
        Indexing,

        /// From building preamble for the source file.
        Preamble,

        /// From building precompiled module for the module interface unit.
        ModuleInterface,

        /// From building normal AST for source file(except preamble), interested file and top level
        /// declarations are available.
        Content,

        /// From running code completion for the source file(preamble is applied).
        Completion,
    };

    using enum Kind;
    struct Impl;

    CompilationUnit(Kind kind, Impl* impl) : kind(kind), impl(impl) {}

    CompilationUnit(const CompilationUnit&) = delete;

    CompilationUnit(CompilationUnit&& other) : kind(other.kind), impl(other.impl) {
        other.impl = nullptr;
    }

    ~CompilationUnit();

public:
    /// Get the file id for given file. If such file doesn't exist, the result
    /// will be invalid file id. If the the content of the file doesn't have
    /// `#pragma once` or guard macro, each inclusion of the file will generate
    /// a new file id, return the first one.
    auto file_id(llvm::StringRef file) -> clang::FileID;

    /// If the location represents file location, it is composed of a file id
    /// and an offset relative to the file begin, decompose it.
    auto decompose_location(clang::SourceLocation location)
        -> std::pair<clang::FileID, std::uint32_t>;

    /// Decompose a source range into file ID and local source range. The begin and end
    /// of the input source range both should be `FileID`. If the range is cross multiple
    /// files, we cut off the range at the end of the first file.
    auto decompose_range(clang::SourceRange range) -> std::pair<clang::FileID, LocalSourceRange>;

    /// Same as `decompose_range`, but will translate range to expansion range.
    auto decompose_expansion_range(clang::SourceRange range)
        -> std::pair<clang::FileID, LocalSourceRange>;

    /// Get the file id of the file location.
    auto file_id(clang::SourceLocation location) -> clang::FileID;

    /// Get the file offset of the file location.
    auto file_offset(clang::SourceLocation location) -> std::uint32_t;

    /// Get the file path of the file ID. If the file exists the path
    /// will be real path, otherwise it will be virtual path. The result
    /// makes sure the path is ended with '/0'.
    auto file_path(clang::FileID fid) -> llvm::StringRef;

    /// Get the file content of the file ID.
    auto file_content(clang::FileID fid) -> llvm::StringRef;

    /// Get the interested file ID. Currently, it is the same as the main
    /// file id，i.e. the file id of source file.
    auto interested_file() -> clang::FileID;

    /// Get the content of interested file.
    auto interested_content() -> llvm::StringRef;

    /// Check if a file is a builtin file.
    bool is_builtin_file(clang::FileID fid);

    /// Get the location of the file start of the file id.
    auto start_location(clang::FileID fid) -> clang::SourceLocation;

    /// Get the location of file end of the file id.
    auto end_location(clang::FileID fid) -> clang::SourceLocation;

    /// Get the include location of the file id, i.e. where the file
    /// was introduced by `#include`.
    auto include_location(clang::FileID fid) -> clang::SourceLocation;

    /// Given a macro location, return its top level spelling location(the location
    /// of the token that the result token is expanded from, may from macro argument
    /// or macro definition).
    auto spelling_location(clang::SourceLocation location) -> clang::SourceLocation;

    /// Given a macro location, return its top level expansion location(the location of
    /// macro expansion).
    auto expansion_location(clang::SourceLocation location) -> clang::SourceLocation;

    ///
    auto file_location(clang::SourceLocation location) -> clang::SourceLocation;

    /// FIXME: Do we really need this function?
    auto presumed_location(clang::SourceLocation location) -> clang::PresumedLoc;

    /// Create a file location with given file id and offset.
    auto create_location(clang::FileID fid, std::uint32_t offset) -> clang::SourceLocation;

    using TokenRange = llvm::ArrayRef<clang::syntax::Token>;

    /// Get the spelled tokens(raw token) of the file id.
    auto spelled_tokens(clang::FileID fid) -> TokenRange;

    /// Return the spelled tokens corresponding to the range.
    auto spelled_tokens(clang::SourceRange range) -> TokenRange;

    /// The spelled tokens that overlap or touch a spelling location Loc.
    /// This always returns 0-2 tokens.
    auto spelled_tokens_touch(clang::SourceLocation location) -> TokenRange;

    /// All tokens produced by the preprocessor after all macro replacements,
    /// directives, etc. Source locations found in the clang AST will always
    /// point to one of these tokens.
    /// Tokens are in TU order (per SourceManager::isBeforeInTranslationUnit()).
    /// FIXME: figure out how to handle token splitting, e.g. '>>' can be split
    ///        into two '>' tokens by the parser. However, TokenBuffer currently
    ///        keeps it as a single '>>' token.
    auto expanded_tokens() -> TokenRange;

    /// Returns the subrange of expandedTokens() corresponding to the closed
    /// token range R.
    auto expanded_tokens(clang::SourceRange range) -> TokenRange;

    auto expansions_overlapping(TokenRange) -> std::vector<clang::syntax::TokenBuffer::Expansion>;

    /// Get the token length.
    auto token_length(clang::SourceLocation location) -> std::uint32_t;

    /// Get the spelling of the token corresponding to the location.
    auto token_spelling(clang::SourceLocation location) -> llvm::StringRef;

    /// Get the C++20 named module name if any.
    auto module_name() -> llvm::StringRef;

    /// Return whether this unit it module interface unit.
    bool is_module_interface_unit();

    /// Return all diagnostics in the process of compilation.
    auto diagnostics() -> llvm::ArrayRef<Diagnostic>;

    auto top_level_decls() -> llvm::ArrayRef<clang::Decl*>;

    clang::LangOptions& lang_options();

    clang::ASTContext& context();

    clang::syntax::TokenBuffer& token_buffer();

    TemplateResolver& resolver();

    llvm::DenseMap<clang::FileID, Directive>& directives();

    clang::TranslationUnitDecl* tu();

    /// All files involved in building the unit.
    const llvm::DenseSet<clang::FileID>& files();

    std::vector<std::string> deps();

    /// Get symbol ID for given declaration.
    index::SymbolID getSymbolID(const clang::NamedDecl* decl);

    /// Get symbol ID for given marco.
    index::SymbolID getSymbolID(const clang::MacroInfo* macro);

private:
    Kind kind;

    Impl* impl;
};

}  // namespace clice
