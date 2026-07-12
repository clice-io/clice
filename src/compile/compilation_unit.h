#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "compile/dep_file.h"
#include "compile/diagnostic.h"
#include "compile/directive.h"
#include "semantic/resolver.h"
#include "syntax/token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice {

namespace index {

// Temporary in-header definition during migration.
struct SymbolID {
    std::uint64_t hash;
    std::string name;
};

}  // namespace index

enum class CompilationKind : std::uint8_t {
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

enum class CompilationStatus : std::uint8_t {
    Completed,
    Cancelled,
    SetupFail,
    FatalError,
};

class CompilationUnitRef {
public:
    struct Self;

    CompilationUnitRef(Self* self) : self(self) {}

    Self* operator->() {
        return self;
    }

public:
    CompilationKind kind();

    CompilationStatus status();

    /// Parse finished; ASTContext is usable but diagnostics may still contain errors.
    bool completed() {
        return status() == CompilationStatus::Completed;
    }

    /// Compilation was cancelled; consumers should not touch any state.
    bool cancelled() {
        return status() == CompilationStatus::Cancelled;
    }

    /// Failed during initial setup; diagnostics exist (location-free), ASTContext
    /// is unavailable.
    bool setup_fail() {
        return status() == CompilationStatus::SetupFail;
    }

    /// Hit an unrecoverable error; diagnostics and decoded source locations
    /// are usable, other states are not unavailable.
    bool fatal_error() {
        return status() == CompilationStatus::FatalError;
    }

public:
    /// Get the file id for given file. If such file doesn't exist, the result
    /// will be invalid file id. If the the content of the file doesn't have
    /// `#pragma once` or guard macro, each inclusion of the file will generate
    /// a new file id, return the first one.
    auto file_id(clang::FileEntryRef file) -> clang::FileID;

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

    /// Get the canonical path of the file entry: absolutized against the
    /// compile's working directory and resolved through the compiler's
    /// VFS, falling back to the absolute path with dot segments removed.
    /// Symlinked spellings of a file resolve to one path; hardlinked
    /// spellings each keep their own. The result is guaranteed to be
    /// null-terminated.
    auto file_path(clang::FileEntryRef entry) -> llvm::StringRef;

    /// Same, for the file entry backing `fid`. The fid must be valid and
    /// backed by a file entry (asserted): synthetic buffers (<built-in>,
    /// <command line>, <scratch space>) have none — callers must filter
    /// them out first, see `is_builtin_file`.
    auto file_path(clang::FileID fid) -> llvm::StringRef;

    /// Get the file content of the file ID.
    auto file_content(clang::FileID fid) -> llvm::StringRef;

    /// The buffer this compilation read for `fid`, or nullopt when it was
    /// never loaded here (e.g. a preamble header served from a PCH). Unlike
    /// file_content, never falls back to a fake buffer.
    auto loaded_file_content(clang::FileID fid) -> std::optional<llvm::StringRef>;

    /// Get the interested file ID. Currently, it is the same as the main
    /// file id，i.e. the file id of source file.
    auto interested_file() -> clang::FileID;

    /// Get the content of interested file.
    auto interested_content() -> llvm::StringRef;

    /// Get the byte offsets of each line start in the interested file.
    /// Lazily computed and cached.
    auto line_starts() -> std::span<const std::uint32_t>;

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
    /// Whether this unit is a named module (interface or implementation).
    /// Must be checked before module_name(): the preprocessor asserts on
    /// name access for non-module units.
    bool is_named_module();

    auto module_name() -> llvm::StringRef;

    /// Return whether this unit it module interface unit.
    bool is_module_interface_unit();

    /// Return all diagnostics in the process of compilation.
    auto diagnostics() -> std::vector<Diagnostic>&;

    auto top_level_decls() -> llvm::ArrayRef<clang::Decl*>;

    std::chrono::milliseconds build_at();

    std::chrono::milliseconds build_duration();

    clang::LangOptions& lang_options();

    clang::ASTContext& context();

    clang::syntax::TokenBuffer& token_buffer();

    TemplateResolver& resolver();

    llvm::DenseMap<clang::FileID, Directive>& directives();

    clang::TranslationUnitDecl* tu();

    /// The files this compilation read (include and __has_include targets),
    /// each with the hash of the bytes the compiler actually consumed.
    std::vector<DepFile> deps();

    /// Get symbol ID for given declaration.
    index::SymbolID getSymbolID(const clang::NamedDecl* decl);

    /// Get symbol ID for given marco.
    index::SymbolID getSymbolID(const clang::MacroInfo* macro);

protected:
    Self* self;
};

/// All AST related information needed for language server.
class CompilationUnit : public CompilationUnitRef {
public:
    explicit CompilationUnit(Self* self) : CompilationUnitRef(self) {}

    CompilationUnit(const CompilationUnit&) = delete;

    CompilationUnit(CompilationUnit&& other) : CompilationUnitRef(other.self) {
        other.self = nullptr;
    }

    CompilationUnit& operator=(const CompilationUnit&) = delete;

    CompilationUnit& operator=(CompilationUnit&& other) {
        std::swap(self, other.self);
        return *this;
    }

    ~CompilationUnit();
};

}  // namespace clice
