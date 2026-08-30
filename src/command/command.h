#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "support/object_pool.h"
#include "support/path_pool.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class StringSaver;

}

namespace clice {

class Toolchain;

enum class CompilerFamily : std::uint8_t {
    Unknown,
    GCC,      // Covers gcc, g++, cc, c++, and versioned/arch variants
    Clang,    // Covers clang, clang++, and versioned variants (excluding clang-cl)
    MSVC,     // Covers cl
    ClangCL,  // Covers clang-cl explicitly, and clang --driver-mode=cl
    NVCC,     // Covers nvcc
    Intel,    // Covers icc, icpc, icx, dpcpp
    Zig,      // Covers zig cc / zig c++ (assumed GCC/Clang compatible for query)
};

/// How an argument participates in compilation semantics. Every parsed
/// argument is kept in the structured command with a class; renders select
/// by class instead of re-parsing.
enum class ArgClass : std::uint8_t {
    /// Affects frontend semantics (language, target, macros defined by mode).
    Semantic,

    /// Per-file user content: -I, -D, -U, -include, -isystem, -iquote,
    /// -idirafter. Excluded from toolchain probes and re-attached after.
    UserContent,

    /// Diagnostics presentation (-W*, -R*, -pedantic*, -w): affects which
    /// warnings are emitted but never the token stream.
    Diagnostics,

    /// Pure backend/linker concerns (-g, -fPIC, -flto, ...): kept for the
    /// Full view, skipped by compile renders and identity hashes.
    Codegen,

    /// Irrelevant to an LSP (outputs, PCH building, dependency scan, module
    /// flags we manage ourselves): kept for the Full view only.
    Discarded,

    /// Unrecognized token, kept verbatim (identity is the spelling). In the
    /// identity hash — dropping an option we don't understand could merge
    /// commands that must differ — but never in compile renders; forwarded
    /// to toolchain probes only for NVCC probe tokens.
    Unknown,

    /// The input slot: where the source file sat in the original command.
    /// A pure position marker — it stores no path (the file is the entry's,
    /// not the config's). Language selectors before it govern the input;
    /// renders inject the actual file here.
    Input,
};

struct Arg {
    std::uint32_t opt_id = 0;
    ArgClass cls = ArgClass::Semantic;

    /// Interned verbatim token, only for ArgClass::Unknown (nullptr otherwise).
    const char* spelling = nullptr;

    /// Interned option values (paths already absolutized for include-path
    /// options). Elements are pointer-comparable.
    llvm::ArrayRef<const char*> values;

    friend bool operator==(const Arg& lhs, const Arg& rhs) {
        return lhs.opt_id == rhs.opt_id && lhs.cls == rhs.cls && lhs.spelling == rhs.spelling &&
               lhs.values == rhs.values;
    }
};

/// A driver-level compile configuration, parsed and classified once at load.
/// Deduped via ObjectSet — identity is the ConfigID. Wrapper prefixes
/// (ccache etc.) are entry provenance, not part of the config: `ccache
/// clang++ X` and `clang++ X` dedupe to one config.
struct CompileConfig {
    /// Working directory (interned).
    const char* directory = nullptr;

    /// argv[0] as invoked (interned). Never realpath'ed: the invoked name
    /// selects driver behavior (clang vs clang++).
    const char* driver = nullptr;

    /// Zig's subcommand token ("cc" / "c++"); nullptr for everything else.
    const char* subcommand = nullptr;

    /// From the driver filename plus an explicit --driver-mode= scan.
    CompilerFamily family = CompilerFamily::Unknown;

    /// Ordered arguments, exactly one ArgClass::Input slot among them.
    llvm::ArrayRef<Arg> args;

    friend bool operator==(const CompileConfig&, const CompileConfig&) = default;
};

enum class ConfigID : std::uint32_t {};

constexpr inline ConfigID invalid_config = ConfigID(~0u);

/// The language dimension of a command for one input file: the clang
/// language name ("c++", "cuda", ...) selected by the governing selector or
/// derived from the file extension; the raw extension itself when no table
/// maps it. Interned — pointer-comparable.
struct InputKind {
    const char* value = nullptr;

    friend bool operator==(const InputKind&, const InputKind&) = default;
};

/// Where the compile command for a file came from. Anything other than
/// CDBExact means the command was guessed to some degree, which is why
/// diagnostics produced with it may deserve a guidance note (see
/// format_diagnostics).
enum class CommandSource : std::uint8_t {
    /// Direct compilation database entry for the file.
    CDBExact,
    /// Header compiled in the context of a host source found through the
    /// include graph (automatic or via clice/switchContext).
    IncludeGraph,
    /// Reserved for command transfer heuristics (e.g. nearest CDB entry);
    /// no producer yet.
    Inferred,
    /// Synthesized default command — no CDB entry and no usable host source.
    Fallback,
};

/// A resolved command selection for one file: the final (rules-applied)
/// config plus the language the toolchain layer probes with. For a header
/// borrowing a host command, `input` is the host's.
struct CommandRef {
    std::uint32_t file = ~0u;
    ConfigID config = invalid_config;
    InputKind input;
    CommandSource source = CommandSource::Fallback;
};

/// Config-rule edits applied on top of a base config (structured, no
/// re-parse). Applied as remove first, then append; appends insert before
/// the input slot so they always take effect for the compile.
struct CommandOptions {
    llvm::ArrayRef<std::string> remove;
    llvm::ArrayRef<std::string> append;

    /// Per-run additions in the resolved command's own dialect (a lint
    /// plan's clang-tool extra args). Unlike the config rule lists above
    /// they are never NVCC-translated — clang-tidy's extra args are clang
    /// args by definition. Prepends land right after the binary name,
    /// ahead of the command's own flags (the command wins on collision);
    /// appends land after the rule appends and win.
    llvm::ArrayRef<std::string> extra_prepend;
    llvm::ArrayRef<std::string> extra_append;

    bool empty() const {
        return remove.empty() && append.empty() && extra_prepend.empty() && extra_append.empty();
    }
};

struct RenderOptions {
    /// Synthesized preamble to inject as `-include <path>`, after the
    /// command's own user-content flags so the host's -include runs first.
    const char* preamble = nullptr;
};

/// A single entry in the compilation database.
struct CompilationEntry {
    /// Path id of the source file (shared PathPool).
    std::uint32_t file = ~0u;

    ConfigID config = invalid_config;

    /// Wrapper prefix stripped at load (ccache, distcc, ...): display
    /// provenance and the last tie-break of candidate ordering. Not part of
    /// config identity.
    llvm::ArrayRef<const char*> wrapper;
};

/// Render one structured argument back into argv fragments. Unknown args
/// render as their verbatim spelling; known options render through the
/// option table (canonical name + render style). Never called on the
/// input slot.
void render_arg(const Arg& arg, llvm::function_ref<void(std::string_view)> cb);

/// The option-table visibility mask of a driver family: CL families see
/// /U-, /D-style options; the rest exclude them so Unix absolute paths
/// are not misparsed.
unsigned family_visibility(CompilerFamily family);

/// Owned copy of a rendered argv (worker IPC and other string-owning edges).
std::vector<std::string> to_strings(llvm::ArrayRef<const char*> argv);

/// Per-file delta of a compilation database reload. Path ids are the shared
/// pool's ids (stable across reloads).
struct CDBDiff {
    /// Files present only after the reload (gained their first entry).
    llvm::SmallVector<std::uint32_t> added;

    /// Files present only before the reload (lost all their entries).
    llvm::SmallVector<std::uint32_t> removed;

    /// Files present on both sides whose set of command hashes differs.
    llvm::SmallVector<std::uint32_t> changed;

    bool empty() const {
        return added.empty() && removed.empty() && changed.empty();
    }
};

class CompilationDatabase {
public:
    CompilationDatabase();
    ~CompilationDatabase();

    CompilationDatabase(const CompilationDatabase&) = delete;
    CompilationDatabase& operator=(const CompilationDatabase&) = delete;

    /// Where probes of cwd-insensitive configs run. Set before load; empty
    /// means the process working directory.
    void set_workspace_root(llvm::StringRef root);

    /// The single path-id space, shared with the whole workspace.
    PathPool& paths() {
        return pool;
    }

    /// Load (or reload) the compilation database from the given file.
    /// On success old entries are replaced, but the pools and configs
    /// survive (path ids stay stable across reloads).
    ///
    /// Parsing is atomic at the top level: if the file cannot be read, is
    /// not valid JSON, or has a root that is not an array, the previously
    /// loaded entries are kept and nullopt is returned. Individual
    /// malformed entries are still skipped — which means a file truncated
    /// mid-array loads as a partial set; the poll-side settle debounce is
    /// what guards against reading half-written files. On success returns
    /// the number of entries loaded.
    std::optional<std::size_t> load(llvm::StringRef path);

    /// Reload the database from `path` and report the per-file delta against
    /// the previously loaded entries.
    ///
    /// Entry identity is the entry hash (Frontend profile + directory), the
    /// same identity the rest of the system uses to pin a CDB entry (e.g.
    /// clice/switchContext). A change confined to codegen-only flags (-g,
    /// -fPIC, -flto, ...) is therefore not reported as `changed` — this is
    /// deliberate. (Optimization level -O* is semantic, not codegen-only: it
    /// defines __OPTIMIZE__, so changing it does count.)
    ///
    /// If `path` cannot be read or does not hold a JSON array, load() keeps
    /// the old entries and nullopt is returned — the caller must retry
    /// rather than treat the failure as "no change".
    std::optional<CDBDiff> reload_and_diff(llvm::StringRef path);

    /// All entries for a file, in deterministic candidate order (the first
    /// is the default selection). Empty when the file has none.
    llvm::ArrayRef<CompilationEntry> candidate_entries(std::uint32_t path_id) const;
    llvm::ArrayRef<CompilationEntry> candidate_entries(llvm::StringRef file);

    bool has_entry(llvm::StringRef file);

    /// All entries, sorted by (file, candidate order).
    llvm::ArrayRef<CompilationEntry> entries() const {
        return entry_list;
    }

    const CompileConfig& config(ConfigID id) const;

    /// Apply config-rule edits to a base config, producing a (deduped)
    /// result config. Structured editing on the arg sequence: remove
    /// cancels matching args, appends insert before the input slot. NVCC
    /// commands translate the edits into clang spellings first, the way
    /// the base command itself was translated. Memoized per
    /// (config, rule set).
    ConfigID apply_rules(ConfigID id, const CommandOptions& options);

    /// The synthesized default command for a file without a CDB entry.
    ConfigID fallback_config(llvm::StringRef file);

    /// Derive the language of `file` compiled under `id`: walk the
    /// language-selector state machine (-x applies to inputs after it,
    /// -x none resets, /TC and /TP set globally) up to the input slot;
    /// with no governing selector, map the file extension.
    InputKind input_kind(ConfigID id, llvm::StringRef file);

    /// The selector-forced language of the config's input, empty when the
    /// extension decides (what input_kind() falls back to).
    llvm::StringRef forced_language(ConfigID id) const;

    /// Identity hash of a config (Frontend view + slot position + directory
    /// + schema salt). The CDB diff identity, the index snapshot command
    /// identity, and — computed over a rules-applied config — the pin
    /// identity of clice/switchContext.
    std::uint64_t entry_hash(ConfigID id);

    /// entry_hash formatted as the persistent/protocol form (16 hex chars).
    std::string entry_hash_hex(ConfigID id);

    /// Map each file's path_id to the sorted entry hashes of its entries (a
    /// file may own several entries with different flags) — the identity
    /// reload_and_diff() diffs on and the indexer persists to catch command
    /// changes across sessions.
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::string, 1>> command_hash_snapshot();

    /// The entry hash of a file's default selection (its first candidate);
    /// nullopt when the file has no entry. Persisted so an offline change
    /// of the winning candidate is detected at startup.
    std::optional<std::string> selected_hash(std::uint32_t path_id);

    /// Render the full compile argv for a ref: resolve the config through
    /// the toolchain (probe cached; may spawn the driver once per unique
    /// toolchain key) and render at cc1 level; on probe failure degrade to
    /// the driver-level render. Pointers are interned and stable for the
    /// database's lifetime.
    std::vector<const char*> render(const CommandRef& ref, const RenderOptions& opts = {});

    /// Driver-level render only (no toolchain probe): the probe-failure
    /// degradation path, and the base of display forms.
    std::vector<const char*> render_driver(const CommandRef& ref, const RenderOptions& opts = {});

    /// Every argument including codegen and discarded ones: the
    /// debug/display form. No file injection.
    std::vector<const char*> render_full(ConfigID id);

    /// Pre-probe the toolchains of the given refs in parallel (deduped by
    /// toolchain key), so later render() calls hit the cache.
    void warm(llvm::ArrayRef<CommandRef> refs);

    /// Header search configuration of a ref, extracted from the resolved
    /// (cc1) config when the toolchain probe succeeds — memoized — else
    /// from the driver-level config.
    SearchConfig search_config(const CommandRef& ref);

    Toolchain& toolchain() {
        return *chain;
    }

#ifdef CLICE_ENABLE_TEST

    /// Append one command and return its entry (candidate order among a
    /// file's accumulated entries is content-based, not insertion-based);
    /// nullopt when normalization fails.
    std::optional<CompilationEntry> add_command(llvm::StringRef directory,
                                                llvm::StringRef file,
                                                llvm::ArrayRef<const char*> arguments);

    std::optional<CompilationEntry> add_command(llvm::StringRef directory,
                                                llvm::StringRef file,
                                                llvm::StringRef command);

#endif

private:
    friend class Toolchain;

    struct NormalizeResult {
        ConfigID config = invalid_config;
        llvm::ArrayRef<const char*> wrapper;
    };

    /// The normalization pipeline (§ wrapper strip → driver info → @rsp
    /// expansion → nvcc translation → parse → classify → path normalize →
    /// dedup). `file` is the entry's normalized path used to pick the input
    /// slot among the command's inputs; ~0u synthesizes the slot at the end.
    std::optional<NormalizeResult> normalize(llvm::StringRef directory,
                                             std::uint32_t file,
                                             llvm::ArrayRef<const char*> arguments);

    std::optional<NormalizeResult> normalize(llvm::StringRef directory,
                                             std::uint32_t file,
                                             llvm::StringRef command);

    /// Expand @file tokens in place, driver-mode aware (CL commands
    /// tokenize with Windows rules).
    void expand_response_files(llvm::SmallVectorImpl<const char*>& tokens,
                               llvm::StringRef directory,
                               CompilerFamily family,
                               llvm::StringSaver& saver,
                               unsigned depth = 0);

    ConfigID save_config(CompileConfig config, llvm::ArrayRef<Arg> local_args);

    llvm::ArrayRef<const char*> persist_strings(llvm::ArrayRef<const char*> values);

    /// Append the Frontend-view fragments of `id` (the hash input) to out.
    void render_identity(ConfigID id, std::string& out);

    /// Stable candidate order within one file: entry hash first, then the
    /// full render + wrapper bytes for hash-equal entries. Content-based —
    /// generator reordering must not change the default selection.
    void sort_entries(std::vector<CompilationEntry>& list);

    std::unique_ptr<llvm::BumpPtrAllocator> allocator = std::make_unique<llvm::BumpPtrAllocator>();

    /// Keep all strings (arguments, directories, etc.).
    StringSet strings{allocator.get()};

    ObjectSet<CompileConfig> configs{allocator.get()};

    /// The workspace's single path-id space (Workspace::path_pool aliases it).
    PathPool pool;

    /// All compilation entries, sorted by (file, candidate order).
    std::vector<CompilationEntry> entry_list;

    std::string workspace_root;

    /// Derivation memos, append-only alongside the pools.
    llvm::DenseMap<std::uint32_t, std::uint64_t> entry_hashes;
    llvm::StringMap<std::uint32_t> rule_set_ids;
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> rule_applied;
    llvm::StringMap<ConfigID> fallback_configs;
    llvm::DenseMap<std::uint32_t, SearchConfig> search_configs;

    std::unique_ptr<Toolchain> chain;
};

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<clice::CompileConfig> {
    using T = clice::CompileConfig;

    inline static T getEmptyKey() {
        return T{.directory = DenseMapInfo<const char*>::getEmptyKey()};
    }

    inline static T getTombstoneKey() {
        return T{.directory = DenseMapInfo<const char*>::getTombstoneKey()};
    }

    static unsigned getHashValue(const T& config) {
        llvm::hash_code hash = llvm::hash_combine(config.directory,
                                                  config.driver,
                                                  config.subcommand,
                                                  static_cast<unsigned>(config.family));
        for(auto& arg: config.args) {
            hash = llvm::hash_combine(hash,
                                      arg.opt_id,
                                      static_cast<unsigned>(arg.cls),
                                      arg.spelling,
                                      llvm::hash_combine_range(arg.values));
        }
        return hash;
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm
