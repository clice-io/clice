module;

#include "modules/prelude.h"

module clice:compile.compilation_unit;

import :compile.dep_file;
import :compile.diagnostic;
import :compile.directive;
import :compile.tokens;
import :semantic.resolver;
import :semantic.symbol;
import :syntax.position;
import :syntax.preamble_synthesis;
import :syntax.token;

namespace clice {

class Semantics;

enum class CompilationKind : std::uint8_t {
    /// From preprocessing the source file. Therefore directives
    /// are available but AST nodes are not.
    Preprocess,

    /// From indexing the static source file.
    Indexing,

    /// From building preamble for the source file.
    Preamble,

    /// From building the precompiled module of a unit providing a module.
    ModuleInterface,

    /// From building normal AST for source file(except preamble), main file and top level
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

    /// Same as `decompose_range`, but will translate range to expansion range:
    /// a range inside a macro expansion covers the whole invocation.
    auto decompose_expansion_range(clang::SourceRange range)
        -> std::pair<clang::FileID, LocalSourceRange>;

    /// Get the file id of the file location.
    auto file_id(clang::SourceLocation location) -> clang::FileID;

    /// Get the file offset of the file location.
    auto file_offset(clang::SourceLocation location) -> std::uint32_t;

    /// The identity of the file entry (CanonicalPath), absolutized against
    /// the compile's working directory: symlinked spellings of a file
    /// resolve to one path; hardlinked spellings each keep their own. The
    /// result is guaranteed to be null-terminated.
    auto file_path(clang::FileEntryRef entry) -> llvm::StringRef;

    /// Same, for the file entry backing `fid`. The fid must be valid and
    /// backed by a file entry (asserted): synthetic buffers (<built-in>,
    /// <command line>, <scratch space>) have none — callers must filter
    /// them out first, see `is_builtin_file`.
    auto file_path(clang::FileID fid) -> llvm::StringRef;

    /// See CompilationParams::workspace.
    auto workspace() -> llvm::StringRef;

    /// Get the file content of the file ID.
    auto file_content(clang::FileID fid) -> llvm::StringRef;

    /// The buffer this compilation read for `fid`, or nullopt when it was
    /// never loaded here (e.g. a preamble header served from a PCH). Unlike
    /// file_content, never falls back to a fake buffer.
    auto loaded_file_content(clang::FileID fid) -> std::optional<llvm::StringRef>;

    /// Return clang's main file ID, the file this unit was built for.
    auto main_file() -> clang::FileID;

    /// Whether `fid` is the main file, or the preamble of it a consumed PCH
    /// recorded — where the preamble's include locations point.
    bool is_main_file(clang::FileID fid);

    /// Get the content of main file.
    auto main_content() -> llvm::StringRef;

    /// A file's positions, over line tables computed on first use and
    /// cached.
    auto positions(clang::FileID fid) -> PositionMap;

    /// The main file's positions.
    auto positions() -> PositionMap;

    /// Check if a file is a builtin file.
    bool is_builtin_file(clang::FileID fid);

    /// Whether the request synthesized the file (a header context's
    /// fragment, see CompilationParams::add_synthesized): no file on disk
    /// carries its bytes, so nothing may depend on it.
    bool synthesized(clang::FileID fid);

    /// Whether the compile borrows an includer context: its main file is
    /// a header, compiled as its host sees it.
    bool borrows_context();

    /// The path of the file `fid` stands for: its own, or for a synthesized
    /// file the one its text was cut from.
    auto source_path(clang::FileID fid) -> llvm::StringRef;

    /// Where `offset` in `fid` lies in source_path(fid): the same offset
    /// but in a synthesized file, whose text runs copy the file at other
    /// offsets.
    std::uint32_t source_offset(clang::FileID fid, std::uint32_t offset);

    /// Whether the file is the compile's own source: the main file, or
    /// under a borrowed includer context a fragment cut from the host.
    bool host_source(clang::FileID fid);

    /// Whether the file belongs to a borrowed includer context: synthesized
    /// itself, or entered through a synthesized file. Such files are the
    /// host's to index, not this unit's.
    bool from_context(clang::FileID fid);

    /// Whether the declaration opens before the main file and closes past
    /// its start: the class, enumeration, namespace or function a fragment
    /// compiled in its includer's context sits inside.
    bool encloses_main_file(const clang::Decl* decl);

private:
    /// What the synthesized file `fid` was cut from; null for any other.
    const SynthesizedOrigin* origin(clang::FileID fid);

public:
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

    /// The token accessors below serve a Content compile, the only kind that
    /// collects tokens; they cover the main file. The spelled tokens are its
    /// raw tokens, the whole file even under a preamble PCH.
    auto spelled_tokens() -> TokenRange;

    /// The spelled tokens the closed token range `range` was produced from,
    /// when it maps onto them exactly (see TokenMap::spelled_for); empty
    /// otherwise.
    auto spelled_tokens(clang::SourceRange range) -> TokenRange;

    /// The spelled tokens that overlap or touch a spelling location Loc.
    /// This always returns 0-2 tokens.
    auto spelled_tokens_touch(clang::SourceLocation location) -> TokenRange;

    /// Per spelled token, whether it was preprocessed away to nothing.
    auto preprocessed_away() -> const std::vector<bool>&;

    /// All tokens produced by the preprocessor after all macro replacements,
    /// directives, etc. Source locations found in the clang AST will always
    /// point to one of these tokens.
    /// Tokens are in TU order (per SourceManager::isBeforeInTranslationUnit()).
    /// FIXME: figure out how to handle token splitting, e.g. '>>' can be split
    ///        into two '>' tokens by the parser, but the stream keeps it as a
    ///        single '>>' token.
    auto expanded_tokens() -> TokenRange;

    /// Returns the subrange of expanded_tokens() corresponding to the closed
    /// token range `range`.
    auto expanded_tokens(clang::SourceRange range) -> TokenRange;

    /// The main file's top-level macro expansions sharing a token with
    /// `spelled`, a range of spelled_tokens().
    auto expansions_overlapping(TokenRange spelled) -> llvm::ArrayRef<MacroExpansion>;

    /// The index into spelled_tokens() of the token expanded_tokens()[index]
    /// stands for, if any (see TokenMap::origin).
    auto expanded_token_origin(std::uint32_t index) -> std::optional<std::uint32_t>;

    /// Get the token length.
    auto token_length(clang::SourceLocation location) -> std::uint32_t;

    /// The text of the token at the location, with line splices and other
    /// physical-source artifacts folded away. `token_length` measures the
    /// physical span instead — the two differ exactly when the token needs
    /// such cleaning.
    auto token_spelling(clang::SourceLocation location) -> std::string;

    /// Whether this unit is a named module (interface or implementation).
    /// Must be checked before module_name(): the preprocessor asserts on
    /// name access for non-module units.
    bool is_named_module();

    /// The C++20 named module name, partition included.
    auto module_name() -> llvm::StringRef;

    /// Whether this unit's module declaration defines the name it declares:
    /// an interface unit's or a partition's, internal ones included. An
    /// implementation unit's (`module M;`) names a module defined elsewhere.
    bool defines_module();

    /// Return all diagnostics in the process of compilation.
    auto diagnostics() -> std::vector<Diagnostic>&;

    auto top_level_decls() -> llvm::ArrayRef<clang::Decl*>;

    std::chrono::milliseconds build_at();

    clang::LangOptions& lang_options();

    clang::ASTContext& context();

    types::TemplateResolver& resolver();

    /// The semantic map of the main file (token → owning node), built
    /// lazily on first use and cached for the unit's lifetime.
    const Semantics& semantics();

    llvm::DenseMap<clang::FileID, Directive>& directives();

    clang::TranslationUnitDecl* tu();

    /// The files this compilation read (include and __has_include targets),
    /// each with the hash of the bytes the compiler actually consumed, and
    /// the places its failed lookups looked (see absent), hash 0.
    std::vector<DepFile> deps();

    /// The places failed include and `__has_include` lookups looked that
    /// hold no file: creating one changes what this compilation would see.
    std::vector<std::string> absent();

    /// The entity of a declaration (semantic/identity.h), memoized for the
    /// unit's lifetime.
    std::uint64_t entity(const clang::NamedDecl* decl);

    /// The entity of a macro definition.
    std::uint64_t entity(const clang::MacroInfo* macro);

    /// The entity of a named module.
    std::uint64_t module_entity(llvm::StringRef name);

    /// The entity of the symbol a declaration is named in (its parent in a
    /// qualified name), 0 at the translation unit.
    std::uint64_t parent(const clang::NamedDecl* decl);

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
