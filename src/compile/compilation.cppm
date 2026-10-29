module clice:compile.compilation;

import :compile.diagnostic;
import :compile.directive;
import :compile.tokens;
import :support.prelude;
import :syntax.header_context;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.source_text;
import :syntax.token;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace tidy {

/// The frozen clang-tidy configuration of one run. Plain data on purpose:
/// it travels in worker requests and in the TURun family's product plan,
/// both of which freeze it at takeoff.
struct TidyParams {
    /// Effective checks glob list in clang-tidy syntax; empty falls back
    /// to the built-in default set.
    std::string checks;

    /// Check options (the .clang-tidy CheckOptions map).
    std::vector<std::pair<std::string, std::string>> options;

    /// Checks whose findings report as errors (WarningsAsErrors globs).
    std::string warnings_as_errors;

    /// Also report findings in headers matching this regex (clang-tidy's
    /// HeaderFilterRegex; empty = focus file only).
    std::string header_filter;

    /// Headers excluded from reporting even when header_filter matches
    /// (clang-tidy's ExcludeHeaderFilterRegex).
    std::string exclude_header_filter;

    /// Also report findings in system headers.
    bool system_headers = false;

    /// What the checks take for headers and for sources by extension;
    /// unset keeps clang-tidy's defaults.
    std::optional<std::vector<std::string>> header_file_extensions;
    std::optional<std::vector<std::string>> implementation_file_extensions;

    /// Extra compiler args from the configuration. -W<group> flags are
    /// consumed engine-side by apply_warning_options so the Checks gate
    /// applies (the clangd approach, see tidy.cpp); a batch lint run
    /// additionally applies the remaining args (command_extra_args) to
    /// its driver command at resolution, as clang-tidy itself does. The
    /// interactive command is never rewritten.
    std::vector<std::string> extra_args;
    std::vector<std::string> extra_args_before;
};

class ClangTidyChecker;

}  // namespace tidy

/// One compile input paired with the hash of the bytes the compiler actually
/// consumed for it — taken from the compiler's own in-memory buffers at build
/// time, never from a later disk read. A later disk read could describe
/// content the build never saw (the file may change while the build runs),
/// which would poison every staleness check built on it.
///
/// hash == 0 means the consumed bytes were not available for hashing; the
/// staleness check treats such an entry conservatively (rebuild once).
struct DepFile {
    std::string path;
    std::uint64_t hash = 0;

    /// A place a failed lookup looked: the build saw no file there.
    bool absent = false;
};

enum class CompilationKind : std::uint8_t {
    /// Only preprocess the source file: directives are available, AST
    /// nodes are not.
    Preprocess,

    /// Build the preamble's PCH into output_file.
    Preamble,

    /// Build the PCH of the whole file into output_file: a precompiled
    /// header the build declares, which other files load.
    PrecompiledHeader,

    /// Build the PCM of a unit providing a module into output_file.
    ModuleInterface,

    /// Parse an open file, over a preamble PCH when one is given: the focus
    /// file's tokens and top-level declarations are collected for the
    /// features, the instantiations Sema made for the index, and clang-tidy
    /// runs in the editor shape.
    Content,

    /// Parse the whole translation unit for the index and lint: nothing is
    /// collected beyond directives, diagnostics and the instantiations Sema
    /// made, and clang-tidy runs in the batch shape.
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

    /// The path the focus file's text is served by, as the editor has it
    /// (add_focus); empty when the compile reads the file on disk or only
    /// a preamble's part of it.
    std::string focus;

    /// The suffix of a header context (add_synthesized). Clang's buffer of
    /// the focus file goes on with its include past the text add_focus
    /// serves.
    std::string suffix;

    /// The files add_synthesized served, by path, with what their text
    /// was cut from; see CompilationUnit::synthesized.
    llvm::StringMap<SynthesizedOrigin> synthesized;

    /// Synthesized files entered after the command's -includes by every
    /// compile but a preamble's (SynthesizedFile::Entry::Forced).
    std::vector<std::string> forced_includes;

    /// The file a header context's prefix was cut from, the host (see
    /// add_synthesized and CompilationUnit::host_source).
    std::string host;

    /// The PCH to load, and how many bytes of the focus file it stands for:
    /// a preamble's, or with no bytes a precompiled header the build
    /// declares, which a preamble's PCH built over it chains onto.
    std::pair<std::string, std::uint32_t> pch;

    /// The PCMs to load, by module name.
    llvm::StringMap<std::string> pcms;

    /// Serve the focus file's text from memory, as the editor has it.
    void add_focus(llvm::StringRef path, llvm::StringRef text) {
        focus = path.str();
        add_remapped_file(path, text);
    }

    /// Serve a header context's synthesized files from memory.
    void add_synthesized(const SynthesizedFiles& files) {
        for(auto& file: files) {
            add_remapped_file(file.path, file.content);
            // Named the way CompilationUnit::file_path names it.
            synthesized[CanonicalPath(Spelling::absolute(file.path)).str()] = file.origin;
            if(file.entry == SynthesizedFile::Entry::Prefix) {
                host = file.origin.source;
            } else if(file.entry == SynthesizedFile::Entry::Forced) {
                forced_includes.push_back(file.path);
            }
        }
        suffix = suffix_path(files).str();
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

class CompilationUnit;

/// One compile: what it reads, which kind it is, and what else runs over
/// the parse.
struct CompilationParams {
    CompilationKind kind;

    CompilationInput input;

    /// Preamble, PrecompiledHeader and ModuleInterface: where the PCH or
    /// the PCM is written.
    std::string output_file;

    struct Completion {
        /// Where in the focus file.
        std::uint32_t offset = 0;

        /// Makes what clang hands the results to, for the unit once it has
        /// opened the focus file; the compile owns it.
        std::function<std::unique_ptr<clang::CodeCompleteConsumer>(CompilationUnit&)> consumer;
    };

    /// Completion: the point and its consumer.
    Completion completion;

    /// Run clang-tidy over the parse with this frozen configuration, in
    /// the shape the kind picks (see CompilationKind).
    std::optional<tidy::TidyParams> tidy;

    /// Skip the bodies of the functions in the files the focus file's
    /// preamble enters (CompilationUnit::in_preamble), but those of
    /// templates likely to forward their arguments: the editor's compiles.
    /// Nothing in a skipped body is reported or instantiated, and so
    /// neither is a declaration being unused, which such a body may use.
    bool skip_bodies = false;

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
    /// an unreadable focus file, or a PCH or PCM rejected while loading.
    /// Diagnostics exist, mostly without a location; the AST does not.
    SetupFail,
};

/// A place in one of the files of a compile.
struct FileOffset {
    clang::FileID file;

    std::uint32_t offset = 0;

    friend bool operator==(const FileOffset&, const FileOffset&) = default;
};

/// A stretch of text in one of the files of a compile.
struct FileRange {
    clang::FileID file;

    LocalSourceRange range;
};

class FocusFile;

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

    auto kind() -> CompilationKind {
        return params.kind;
    }

    bool skips_bodies() {
        return params.skip_bodies;
    }

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

    /// Preamble, PrecompiledHeader and ModuleInterface: whether clang
    /// produced the output file. It is written when the unit is destroyed.
    bool has_output();

    /// Get the file id for given file. If such file doesn't exist, the result
    /// will be invalid file id. If the the content of the file doesn't have
    /// `#pragma once` or guard macro, each inclusion of the file will generate
    /// a new file id, return the first one.
    auto file_id(clang::FileEntryRef file) -> clang::FileID;

    auto file_id(llvm::StringRef file) -> clang::FileID;

    /// The file `location` is in; for a location a macro produced, the
    /// file of its expansion record.
    auto file_id(clang::SourceLocation location) -> clang::FileID;

    /// Where the token at `location` is written. Of `#define CALL(f) f(1) + 0`
    /// and `CALL(helper);`, `helper` is written in the argument and `(1)`
    /// in the `#define`; a token a `##` pasted, in the scratch buffer.
    auto written_at(clang::SourceLocation location) -> FileOffset;

    /// Where the user sees `location`: a macro argument where it is
    /// written, anything else a macro produced at the start of the
    /// invocation. Of the example above, `helper` in the argument, `(1)` at
    /// `CALL`.
    auto seen_at(clang::SourceLocation location) -> FileOffset;

    /// Where the macro invocation that produced `location` starts, the
    /// location itself outside macros: all of `helper(1) + 0` at `CALL`.
    auto produced_at(clang::SourceLocation location) -> FileOffset;

    /// The text `range` was written as, token for token: a macro argument
    /// its tokens, a whole expansion the invocation; none when the range
    /// takes in only part of a macro's own tokens (`helper(1)` above) or
    /// runs across files. In the focus file it is read off the tokens a Content
    /// compile collects, elsewhere off clang's own mapping.
    auto written_range(clang::SourceRange range) -> std::optional<FileRange>;

    /// The text of the macro invocations that produced `range`, from the
    /// one its begin is in to the one its end is in; outside macros the
    /// range itself. Of the example above, all of `CALL(helper)` for
    /// `helper(1)` and for `helper` alike. A range running across files
    /// stops at the end of the first.
    auto produced_range(clang::SourceRange range) -> FileRange;

    /// The file this compile serves.
    auto focus_file() -> FocusFile;

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

    /// The text of `fid`: the focus file's as the editor has it, any
    /// other's as clang read it. In the focus file every offset the unit
    /// reports or takes is one in this text, though clang's buffer of it
    /// holds a NUL at a completion's point and goes on with the include of
    /// a header context's suffix.
    auto file_content(clang::FileID fid) -> llvm::StringRef;

    /// The buffer this compilation read for `fid`, or nullopt when it was
    /// never loaded here (e.g. a preamble header served from a PCH). Unlike
    /// file_content, never falls back to a fake buffer.
    auto loaded_file_content(clang::FileID fid) -> std::optional<llvm::StringRef>;

    /// The positions of file_content(fid).
    auto positions(clang::FileID fid) -> PositionMap;

    /// The comments of file_content(fid).
    auto comments(clang::FileID fid) -> llvm::ArrayRef<LexicalInfo::Comment>;

    /// The lexical scan of file_content(fid).
    auto lexical(clang::FileID fid) -> const LexicalInfo&;

    /// Check if a file is a builtin file.
    bool is_builtin_file(clang::FileID fid);

    /// Whether the request synthesized the file (a header context's
    /// fragment, see CompilationInput::add_synthesized): no file on disk
    /// carries its bytes, so nothing may depend on it.
    bool synthesized(clang::FileID fid);

    /// Whether the compile borrows an includer context: its focus file is
    /// a header, compiled as its host sees it.
    bool borrows_context();

    /// The path of the file `fid` stands for: its own, or for a synthesized
    /// file the one its text was cut from.
    auto source_path(clang::FileID fid) -> llvm::StringRef;

    /// Where `offset` in `fid` lies in source_path(fid): the same offset
    /// but in a synthesized file, whose text runs copy the file at other
    /// offsets.
    std::uint32_t source_offset(clang::FileID fid, std::uint32_t offset);

    /// Whether the file is the compile's own source: the focus file, or
    /// under a borrowed includer context a fragment cut from the host.
    bool host_source(clang::FileID fid);

    /// Whether the file belongs to a borrowed includer context: synthesized
    /// itself, or entered through a synthesized file. Such files are the
    /// host's to index, not this unit's.
    bool from_context(clang::FileID fid);

    /// Whether the declaration opens before the focus file and closes past
    /// its start: the class, enumeration, namespace or function a fragment
    /// compiled in its includer's context sits inside.
    bool encloses_focus_file(const clang::Decl* decl);

    /// Whether the file is one the focus file's preamble enters, directly
    /// or not: one of the command's -includes, a header context's prefix
    /// among them, or a file the focus file includes before its preamble
    /// ends. A PCH build's focus file is a preamble whole. The part of a
    /// header context inside the braces the header sits in comes after the
    /// preamble.
    bool in_preamble(clang::FileID fid);

    /// Get the include location of the file id, i.e. where the file
    /// was introduced by `#include`.
    auto include_location(clang::FileID fid) -> clang::SourceLocation;

    /// The location's line and column in its file, `#line` directives ignored.
    auto presumed_location(clang::SourceLocation location) -> clang::PresumedLoc;

    /// Create a file location with given file id and offset.
    auto create_location(clang::FileID fid, std::uint32_t offset) -> clang::SourceLocation;

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

    /// The definitions of the macro `name`, in the order the preprocessor
    /// met them: those of the preamble the compile loads too, which no
    /// directive of this compile records.
    auto macro_definitions(llvm::StringRef name) -> llvm::SmallVector<const clang::MacroInfo*>;

    /// The diagnostics the compile reported, each with its notes: the
    /// compiler's, and clang-tidy's findings in the editor shape. Those a
    /// suppression comment hides are left out, and a finding reported more
    /// than once is kept once.
    auto diagnostics() -> llvm::ArrayRef<Diagnostic>;

    /// The findings of a batch clang-tidy run, the compiler's diagnostics
    /// among them, as clang-tidy reports them; taken once.
    auto take_tidy_findings() -> std::vector<clang::tidy::ClangTidyError>;

    auto top_level_decls() -> llvm::ArrayRef<clang::Decl*>;

    /// The definitions a Whole or Content compile instantiated from
    /// templates, implicitly or for an explicit instantiation — function
    /// bodies, variable initializers and classes — in the order Sema
    /// finished them; empty for every other kind. A Content compile behind
    /// a preamble sees the function bodies the preamble left pending, not
    /// the classes it instantiated.
    auto instantiations() -> llvm::ArrayRef<const clang::Decl*>;

    std::chrono::milliseconds build_at();

    clang::LangOptions& lang_options();

    /// The keywords of the compile's dialect, its own flags' included
    /// (`-fms-extensions`): the preprocessor's identifier table.
    auto keywords() -> const clang::IdentifierTable&;

    clang::ASTContext& context();

    llvm::DenseMap<clang::FileID, Directive>& directives();

    clang::TranslationUnitDecl* tu();

    /// The files this compilation read (include and __has_include targets),
    /// each with the hash of the bytes the compiler actually consumed, and
    /// the places its failed lookups looked (see absent), hash 0.
    std::vector<DepFile> deps();

    /// The places failed include and `__has_include` lookups looked that
    /// hold no file: creating one changes what this compilation would see.
    std::vector<std::string> absent();

    /// What a reader derives from the AST, such as the semantic map or the
    /// entity table: built as `T(unit)` on first use and kept until the
    /// unit is destroyed, then released before the AST.
    template <typename T>
    T& get() {
        for(auto& entry: derived) {
            if(entry.key == &derived_key<T>) {
                return static_cast<Derived<T>&>(*entry.data).value;
            }
        }
        // Building one may get another: store it only once it is built.
        auto data = std::make_unique<Derived<T>>(*this);
        auto& value = data->value;
        derived.push_back({&derived_key<T>, std::move(data)});
        return value;
    }

private:
    friend std::unique_ptr<CompilationUnit> compile(CompilationParams params);
    friend class FocusFile;

    class DiagnosticRecorder;
    class DirectiveRecorder;
    class ParseConsumer;
    class ParseAction;

    explicit CompilationUnit(CompilationParams params);

    /// The consumer filling collected_diagnostics, see diagnostic.cpp.
    std::unique_ptr<clang::DiagnosticConsumer> diagnostic_recorder();

    /// The callbacks filling collected_directives and absent_paths, see
    /// directive.cpp.
    std::unique_ptr<clang::PPCallbacks> directive_recorder();

    auto SM() -> clang::SourceManager& {
        return instance->getSourceManager();
    }

    auto file_text(clang::FileID fid) -> SourceText&;

    /// Where the file location `location` lies (see file_content).
    auto decompose(clang::SourceLocation location) -> FileOffset;

    /// The parser splits a `>>` closing two template argument lists in two:
    /// the first `>` becomes a macro location over its half of the token.
    auto split_half(clang::SourceLocation location) -> std::optional<FileRange>;

    /// What the synthesized file `fid` was cut from; null for any other.
    const SynthesizedOrigin* origin(clang::FileID fid);

    /// Create the `clang::CompilerInvocation` of the compile: the command,
    /// with every flag clice needs set and every one it must not see reset.
    std::unique_ptr<clang::CompilerInvocation>
        create_invocation(clang::DiagnosticConsumer* consumer);

    CompilationStatus run();

    /// Must be called before EndSourceFile because the ast context can be
    /// destroyed later.
    void run_tidy();

    struct DerivedBase {
        virtual ~DerivedBase() = default;
    };

    template <typename T>
    struct Derived : DerivedBase {
        explicit Derived(CompilationUnit& unit) : value(unit) {}

        T value;
    };

    struct DerivedEntry {
        const void* key;

        std::unique_ptr<DerivedBase> data;
    };

    /// One per type, its address the type's key in `derived`. Not const:
    /// the linker may fold equal constants into one address.
    template <typename T>
    inline static char derived_key = 0;

    CompilationParams params;

    CompilationStatus outcome = CompilationStatus::SetupFail;

    /// See fatal_error.
    bool fatal = false;

    /// See has_output.
    bool output_ready = false;

    /// Preamble and PrecompiledHeader: whether the PCH instantiates the
    /// templates its code uses; compile() clears it to build again when
    /// that raised an error.
    bool instantiate = true;

    /// Preamble and PrecompiledHeader: how many errors clang had raised
    /// when the parse reached the end of the focus file.
    std::optional<unsigned> errors_at_end;

    /// When the compile started.
    std::chrono::milliseconds build_time;

    /// Every place a failed include or `__has_include` lookup looked, see
    /// CompilationUnit::absent.
    llvm::StringSet<> absent_paths;

    /// Memo of CompilationUnit::from_context.
    llvm::DenseMap<clang::FileID, bool> context_files;

    /// Memo of CompilationUnit::in_preamble.
    llvm::DenseMap<clang::FileID, bool> preamble_files;

    /// Where the focus file's preamble ends (in_preamble), found on first
    /// use.
    std::optional<std::uint32_t> preamble_end;

    /// The FileID of each file, as DirectiveRecorder saw the unit first
    /// enter it or SourceManager::translateFile found it: translateFile
    /// scans every SLocEntry, loading a precompiled header's, and a guarded
    /// header asks for it again on each include it skips.
    llvm::DenseMap<const clang::FileEntry*, clang::FileID> file_ids;

    /// The buffers clang reads params.input.buffers through; clang leaves
    /// them to their owner (RetainRemappedFileBuffers).
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> remapped_buffers;

    /// The frontend action used to build the unit.
    std::unique_ptr<clang::FrontendAction> action;

    /// Compiler instance, responsible for performing the actual compilation and managing the
    /// lifecycle of all objects during the compilation process.
    std::unique_ptr<clang::CompilerInstance> instance;

    /// See get, in the order they were built.
    llvm::SmallVector<DerivedEntry, 4> derived;

    /// The tokens a Content compile collects for the features.
    std::optional<TokenMap> collected_tokens;

    /// All directive information collected during the preprocessing.
    llvm::DenseMap<clang::FileID, Directive> collected_directives;

    /// Cache for file path. It is used to avoid multiple file path lookup.
    llvm::DenseMap<clang::FileEntryRef, llvm::StringRef> path_cache;

    /// Each file's text (file_content), made on first use.
    llvm::DenseMap<clang::FileID, std::unique_ptr<SourceText>> texts;

    llvm::BumpPtrAllocator path_storage;

    std::vector<Diagnostic> collected_diagnostics;

    std::vector<clang::Decl*> collected_decls;

    std::vector<const clang::Decl*> collected_instantiations;

    std::unique_ptr<tidy::ClangTidyChecker> checker;
};

/// The file a compile serves: the one the user opened, a header compiled
/// in its host's context too. The preamble a consumed PCH holds is part of
/// it, at the same offsets.
class FocusFile {
public:
    explicit FocusFile(CompilationUnit& unit) : unit(unit) {}

    auto file() -> clang::FileID;

    /// Whether `fid` is this file or the preamble of it a consumed PCH
    /// recorded, where the preamble's directives point.
    bool contains(clang::FileID fid);

    /// The text itself, for what reads a SourceText whole.
    auto text() -> SourceText&;

    auto content() -> llvm::StringRef;

    auto positions() -> PositionMap;

    /// The tokens a Content compile collects, the only kind that collects
    /// them.
    auto tokens() -> const TokenMap&;

    /// The lexical scan of the text, run on first use: the one every
    /// feature reads its raw tokens, comments and directive lines from.
    auto lexical() -> const LexicalInfo&;

    /// What `token`, one of lexical()'s, lexes as (SourceText::kind).
    auto kind(const Token& token) -> TokenKind;

    /// The location at `offset`.
    auto location(std::uint32_t offset) -> clang::SourceLocation;

    /// CompilationUnit::seen_at, when it is in this file.
    auto offset(clang::SourceLocation location) -> std::optional<std::uint32_t>;

    /// CompilationUnit::written_range, when it is in this file.
    auto written_range(clang::SourceRange range) -> std::optional<LocalSourceRange>;

    /// CompilationUnit::produced_range, when it is in this file.
    auto produced_range(clang::SourceRange range) -> std::optional<LocalSourceRange>;

private:
    CompilationUnit& unit;
};

/// How compile() builds a PCH: its inputs are checked by size alone when
/// it is loaded, the templates its preamble uses are instantiated in it
/// unless that raises an error, template bodies are deferred only when the
/// command asks, and function bodies are skipped under skip_bodies. Keys
/// of cached PCHs include it, so a blob built under another rule is never
/// reused.
constexpr llvm::StringRef pch_build_rule(bool skip_bodies) {
    return skip_bodies ? "size,instantiated,undeferred,skipped" : "size,instantiated,undeferred";
}

/// The warnings that a declaration is unused: a use in a body skip_bodies
/// skipped goes unseen, so a compile under it raises them for the
/// declarations only such a body uses too.
constexpr inline std::array<unsigned, 6> unused_declaration_warnings = {
    clang::diag::warn_unused_function,
    clang::diag::warn_unused_member_function,
    clang::diag::warn_unused_template,
    clang::diag::warn_unneeded_internal_decl,
    clang::diag::warn_unneeded_static_internal_decl,
    clang::diag::warn_unneeded_member_function,
};

/// The messages of a finished compilation's error-level diagnostics, in
/// the order clang raised them, but for the unused declarations a compile
/// skipping bodies raises, which -Werror may have made errors.
std::vector<std::string> collect_errors(CompilationUnit& unit);

}  // namespace clice
