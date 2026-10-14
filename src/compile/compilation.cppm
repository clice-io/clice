module;

#include "modules/prelude.h"

module clice:compile.compilation;

import :compile.dep_file;
import :compile.diagnostic;
import :compile.directive;
import :compile.tidy;
import :compile.tokens;
import :semantic.resolver;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.preamble_synthesis;
import :syntax.token;
import :vfs.file_system;
import :vfs.path;

namespace clice {

class EntityTable;
class Semantics;

enum class CompilationKind : std::uint8_t {
    /// Only preprocess the source file: directives are available, AST
    /// nodes are not.
    Preprocess,

    /// Build the preamble's PCH into output_file.
    Preamble,

    /// Build the PCM of a unit providing a module into output_file.
    ModuleInterface,

    /// Parse an open file, over a preamble PCH when one is given: the main
    /// file's tokens and top-level declarations are collected for the
    /// features, and clang-tidy runs in the editor shape.
    Content,

    /// Parse the whole translation unit for the index and lint: nothing is
    /// collected beyond directives and diagnostics, and clang-tidy runs in
    /// the batch shape.
    Whole,

    /// Run code completion at the point CompilationParams::completion
    /// names.
    Completion,
};

/// What a compile reads: its command, the files it is served from memory
/// and the precompiled files it loads. Copied when the same input compiles
/// again.
struct CompilationInput {
    /// The compile command.
    std::vector<std::string> arguments;

    /// The directory the command runs in.
    std::string directory;

    /// The workspace root: hashes that outlive the checkout's location
    /// name the unit's files relative to it (path::portable). Empty for
    /// none.
    std::string workspace;

    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = new vfs::View();

    /// The text of the files served from memory, by path.
    llvm::StringMap<std::string> buffers;

    /// The files add_synthesized served, by path, with what their text
    /// was cut from; see CompilationUnit::synthesized.
    llvm::StringMap<SynthesizedOrigin> synthesized;

    /// Synthesized files entered after the command's -includes by every
    /// compile but a preamble's (SynthesizedFile::forced).
    std::vector<std::string> forced_includes;

    /// The preamble PCH to load, and how many bytes of the main file it
    /// stands for.
    std::pair<std::string, std::uint32_t> pch;

    /// The PCMs to load, by module name.
    llvm::StringMap<std::string> pcms;

    /// Serve files the command names from memory: a header context's
    /// synthesized fragments.
    void add_synthesized(const SynthesizedFiles& files) {
        for(auto& file: files) {
            add_remapped_file(file.path, file.content);
            // Named the way CompilationUnit::file_path names it.
            synthesized[CanonicalPath(Spelling::absolute(file.path)).str()] = file.origin;
            if(file.origin.forced) {
                forced_includes.push_back(file.path);
            }
        }
    }

    void add_remapped_file(llvm::StringRef path,
                           llvm::StringRef content,
                           std::uint32_t bound = -1) {
        if(bound != -1) {
            assert(bound <= content.size());
            content = content.substr(0, bound);
        }
        buffers.try_emplace(path, content.str());
    }
};

/// One compile: what it reads, which kind it is, and what else runs over
/// the parse.
struct CompilationParams {
    CompilationKind kind;

    CompilationInput input;

    /// Preamble and ModuleInterface: where the PCH or the PCM is written.
    std::string output_file;

    struct Completion {
        /// Where in the main file.
        std::uint32_t offset = 0;

        /// What clang hands the results to; the compile owns it.
        std::unique_ptr<clang::CodeCompleteConsumer> consumer;
    };

    /// Completion: the point and its consumer.
    Completion completion;

    /// Run clang-tidy over the parse with this frozen configuration, in
    /// the shape the kind picks (see CompilationKind).
    std::optional<tidy::TidyParams> tidy;

    /// Inactive regions of the consumed PCH's preamble share (flat
    /// begin/end offset pairs): the `#include`s there were never entered.
    std::vector<std::uint32_t> preamble_inactive_regions;

    /// Set to stop the compile, an old one a newer request replaces.
    std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);
};

enum class CompilationStatus : std::uint8_t {
    /// The parse ran to the end; the AST is usable, though diagnostics may
    /// hold errors.
    Completed,

    /// The compile was stopped; nothing in the unit is to be used.
    Cancelled,

    /// Clang never started parsing: an invalid command, an unknown target,
    /// an unreadable main file, or a PCH or PCM rejected while loading.
    /// Diagnostics exist, mostly without a location; the AST does not.
    SetupFail,
};

class CompilationUnit;

/// Run the compile `params` describes. The caller checks that the PCH and
/// the PCMs it hands in are still fresh.
std::unique_ptr<CompilationUnit> compile(CompilationParams params);

/// The AST of one compile, what clang reported while it ran, and the
/// helpers that read them.
class CompilationUnit {
public:
    CompilationUnit(const CompilationUnit&) = delete;
    CompilationUnit& operator=(const CompilationUnit&) = delete;

    ~CompilationUnit();

    bool completed() {
        return outcome == CompilationStatus::Completed;
    }

    bool setup_fail() {
        return outcome == CompilationStatus::SetupFail;
    }

    /// The parse went on past a fatal error: clang reported nothing after
    /// it and instantiated no more templates, so the AST stops short. A
    /// PCH or PCM that turns out unreadable halfway through, or a module
    /// that fails to load, ends a parse this way.
    bool fatal_error();

    /// Preamble and ModuleInterface: whether clang produced the output
    /// file. It is written when the unit is destroyed.
    bool has_output();

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

    /// See CompilationInput::workspace.
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

    /// A file's comments, lexed on first use and cached.
    auto comments(clang::FileID fid) -> llvm::ArrayRef<LexicalInfo::Comment>;

    /// Check if a file is a builtin file.
    bool is_builtin_file(clang::FileID fid);

    /// Whether the request synthesized the file (a header context's
    /// fragment, see CompilationInput::add_synthesized): no file on disk
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

    /// The tokens a Content compile collects, the only kind that collects
    /// them.
    auto tokens() -> const TokenMap&;

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
    auto diagnostics() -> llvm::ArrayRef<Diagnostic>;

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

private:
    friend std::unique_ptr<CompilationUnit> compile(CompilationParams params);

    class DiagnosticCollector;
    class DirectiveCollector;
    class ParseConsumer;
    class ParseAction;

    explicit CompilationUnit(CompilationParams params);

    /// The consumer filling collected_diagnostics, see diagnostic.cpp.
    std::unique_ptr<clang::DiagnosticConsumer> diagnostic_collector();

    /// The callbacks filling collected_directives and absent_paths, see
    /// directive.cpp.
    std::unique_ptr<clang::PPCallbacks> directive_collector();

    auto SM() -> clang::SourceManager& {
        return instance->getSourceManager();
    }

    /// What the synthesized file `fid` was cut from; null for any other.
    const SynthesizedOrigin* origin(clang::FileID fid);

    /// Create the `clang::CompilerInvocation` of the compile: the command,
    /// with every flag clice needs set and every one it must not see reset.
    std::unique_ptr<clang::CompilerInvocation> create_invocation(clang::DiagnosticConsumer* consumer);

    CompilationStatus run();

    /// Must be called before EndSourceFile because the ast context can be
    /// destroyed later.
    void run_tidy();

    CompilationParams params;

    CompilationStatus outcome = CompilationStatus::SetupFail;

    /// See fatal_error.
    bool fatal = false;

    /// See has_output.
    bool output_ready = false;

    /// When the compile started.
    std::chrono::milliseconds build_time;

    /// Every place a failed include or `__has_include` lookup looked, see
    /// CompilationUnit::absent.
    llvm::StringSet<> absent_paths;

    /// Memo of CompilationUnit::from_context.
    llvm::DenseMap<clang::FileID, bool> context_files;

    /// Memo of the host path CompilationUnit::host_source compares
    /// against.
    std::optional<std::string> host;

    /// The buffers clang reads params.input.buffers through; clang leaves
    /// them to their owner (RetainRemappedFileBuffers).
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> remapped_buffers;

    /// The frontend action used to build the unit.
    std::unique_ptr<clang::FrontendAction> action;

    /// Compiler instance, responsible for performing the actual compilation and managing the
    /// lifecycle of all objects during the compilation process.
    std::unique_ptr<clang::CompilerInstance> instance;

    /// The template resolver used to resolve dependent name.
    std::optional<types::TemplateResolver> template_resolver;

    /// Lazily built semantic map, see CompilationUnit::semantics().
    std::unique_ptr<Semantics> semantics_cache;

    /// The tokens a Content compile collects for the features.
    std::optional<TokenMap> collected_tokens;

    /// All directive information collected during the preprocessing.
    llvm::DenseMap<clang::FileID, Directive> collected_directives;

    /// Cache for file path. It is used to avoid multiple file path lookup.
    llvm::DenseMap<clang::FileEntryRef, llvm::StringRef> path_cache;

    /// Lazily built, see CompilationUnit::entity().
    std::unique_ptr<EntityTable> entity_table;

    struct LineTables {
        std::vector<std::uint32_t> starts;
        std::vector<std::uint64_t> non_ascii;
    };

    /// Each file's line tables, built together on first use.
    llvm::DenseMap<clang::FileID, LineTables> line_tables;

    /// Each file's comments, lexed on first use.
    llvm::DenseMap<clang::FileID, std::vector<LexicalInfo::Comment>> comment_cache;

    llvm::BumpPtrAllocator path_storage;

    std::vector<Diagnostic> collected_diagnostics;

    std::vector<clang::Decl*> collected_decls;

    std::unique_ptr<tidy::ClangTidyChecker> checker;
};

/// How a PCH built by compile() checks its inputs when loaded: by size
/// alone. Keys of cached PCHs include it, so a blob built under another
/// rule is never reused.
constexpr inline llvm::StringRef pch_input_check = "size";

/// The messages of a finished compilation's error-level diagnostics, in
/// the order clang raised them.
std::vector<std::string> collect_errors(CompilationUnit& unit);

}  // namespace clice
