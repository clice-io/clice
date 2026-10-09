module;

#include "modules/prelude.h"

module clice:project.build;

import :command.command;
import :config.config;
import :vfs.file_table;

namespace clice {

/// A command a file compiles under: one of its database entries
/// (CDBExact); for a file without entries that a rule's default command
/// claims, that command (Default); for a provisional member, the command it
/// borrows (Inferred).
struct Candidate {
    ConfigID config;
    CommandSource source;
};

/// The rules-applied edits of a set of files: every matching active rule
/// in declaration order, each once, its removes before its appends.
struct Edits {
    std::vector<CommandEdit> edits;

    bool empty() const {
        return edits.empty();
    }

    CommandOptions options(llvm::ArrayRef<std::string> extra_prepend = {},
                           llvm::ArrayRef<std::string> extra_append = {}) const {
        return {.edits = edits, .extra_prepend = extra_prepend, .extra_append = extra_append};
    }
};

/// The build: which of the database's entries and which hand-written
/// commands apply to a file under the active configuration, and how the
/// rules edit them — a function of the configuration, the database and
/// the files recorded as provisional members, the one place that knows
/// rule priority. The database stores entries per source in file order
/// and nothing else consults the rules.
class Build {
public:
    Build(Config& config, CompilationDatabase& cdb, FileTable& files) :
        config(config), cdb(cdb), files(files) {}

    /// The configuration tag every tagged rule is checked against; empty
    /// when the rules declare none.
    llvm::StringRef active_configuration() const {
        return active;
    }

    /// Activate a configuration — a declared tag, or empty when the rules
    /// declare none (see resolve_configuration) — and forget what the
    /// previous one enumerated and recorded: the default sources and the
    /// provisional members.
    void reset_active(llvm::StringRef configuration);

    /// Whether an active rule declares a command source — a database or a
    /// default command. Declared sources turn automatic discovery off.
    bool declares_sources() const;

    /// The databases the active view compiles from, in priority order:
    /// the sources of every matching-or-not active rule, deduplicated.
    /// Empty when no rule declares one — discovery's cue.
    llvm::SmallVector<Spelling> declared_sources() const;

    /// Every registered source in the priority order `path` sees: the
    /// sources of rules matching the file first, then those of the other
    /// active rules, each in declaration order; sources no active rule
    /// declares (discovered ones) last — those still on disk before the
    /// vanished, shallower before deeper, then by path. While the active
    /// configuration declares sources, one only inactive rules declare is
    /// left out.
    llvm::SmallVector<SourceID, 4> source_order(CanonicalRef path) const;

    /// Whether discovery registered the source: no active rule declares it
    /// (under discovery every registered source is discovered).
    bool discovered(SourceID id) const;

    /// A file's database entries in build order: entries from the sources
    /// of rules matching the file first, then from the other active rules'
    /// sources, each in declaration order; discovered sources (registered
    /// without a rule) last; within a source, file order. The first is the
    /// default selection. Entries of sources only inactive rules declare
    /// are excluded.
    llvm::SmallVector<CompilationEntry, 2> entries(Fid file) const;

    /// The commands a file compiles under, in build order: its entries, or
    /// the default command of the first matching rule that declares one, or
    /// the command a provisional member borrows. The first is the default
    /// selection; empty when the file has none — such a file is never a
    /// header's host.
    llvm::SmallVector<Candidate, 2> commands(Fid file);

    /// The edits the rules matching any of `paths` contribute, in
    /// declaration order, each rule once — a header borrowing a host's
    /// command passes both so it inherits the host's edits.
    Edits edits(llvm::ArrayRef<CanonicalRef> paths) const;

    Edits edits(CanonicalRef path) const {
        return edits(llvm::ArrayRef(path));
    }

    /// The builtin fallback command of a file the build does not compile
    /// (CommandSource::Fallback): the driver follows the language clang
    /// assigns to the file's extension, an ambiguous `.h` counting as C++.
    ConfigID builtin(CanonicalRef path);

    /// Apply the edits of `paths` (and a run's extras) to `base`: the
    /// effective command of `file`, whose language is derived from the
    /// edited command and `language_path` (the file itself, or the host a
    /// header borrows from).
    CommandRef resolve(Fid file,
                       ConfigID base,
                       CommandSource source,
                       llvm::ArrayRef<CanonicalRef> paths,
                       llvm::StringRef language_path,
                       llvm::ArrayRef<std::string> extra_prepend = {},
                       llvm::ArrayRef<std::string> extra_append = {});

    /// Hash of the edits for `paths`, empty when none apply: the rules'
    /// part of a file's persisted command identity, the workspace root
    /// taken as `${workspace}`.
    std::string edit_hash(llvm::ArrayRef<CanonicalRef> paths) const;

    /// Every translation unit of the build: files with entries, plus the
    /// source files on disk that a default-command rule matches — enumerated
    /// once per active configuration and again by refresh_default_sources —
    /// then the provisional members.
    std::vector<Fid> members();

    /// Record that the user saved `file`, a source the build does not
    /// declare: a provisional member while it has a lender and no host (see
    /// Project::refresh_provisional), until an entry or a rule claims it.
    /// False when it is recorded already.
    bool record(Fid file);

    /// Drop the record of `file`, and its borrowed command.
    void forget(Fid file);

    /// The files the user saved that the build does not declare, unordered:
    /// persisted with the index, so the members they make outlive the
    /// session.
    const llvm::DenseSet<Fid>& recorded() const {
        return records;
    }

    /// Moves whenever recorded() changes, so persistence can tell.
    std::uint64_t recorded_generation() const {
        return records_generation;
    }

    /// Set the command a recorded file borrows as a provisional member,
    /// nullopt while it is none; returns the one it borrowed before.
    std::optional<ConfigID> borrow(Fid file, std::optional<ConfigID> command);

    /// The command `file` borrows from `lender` compiling under `command`:
    /// the edits of the rules matching the lender but not the file applied,
    /// so rendering it as the file's own command, which applies the file's
    /// rules, edits it for both, each rule once.
    ConfigID lend(ConfigID command, CanonicalRef lender, CanonicalRef file);

    /// The directories the default-command rules claim sources under, none
    /// inside another, and the cache directory a walk of them skips.
    struct SourceWalk {
        llvm::SmallVector<CanonicalPath> roots;
        /// The roots of rules with patterns: a file there may be claimed
        /// whatever its suffix (a forced language), elsewhere only one
        /// clang recognizes.
        llvm::SmallVector<CanonicalPath> patterned;
        CanonicalPath cache_dir;
    };

    /// What a default-sources enumeration walks; no roots when no active
    /// rule claims sources.
    SourceWalk source_walk() const;

    /// Enumerate the sources the default-command rules claim again, from a
    /// walk of source_walk() (see walk_sources), and report the ones that
    /// appeared since the last enumeration — a file created after startup
    /// joins the build; a deleted one just leaves the members. The file
    /// tracker calls it every workspace poll.
    llvm::SmallVector<Fid> refresh_default_sources(llvm::ArrayRef<CanonicalPath> walked);

    /// The scan units of `members`: every command of every member, so a
    /// header reachable through only one of a file's entries still finds
    /// that host.
    llvm::SmallVector<CommandRef> units(llvm::ArrayRef<Fid> members);

    /// Whether `file` is a translation unit of its own: declared() or a
    /// provisional member — the members() filter for one file, so
    /// commands() is never empty for a unit. A header claims no unit; its
    /// default command is the resolver's last resort after host inference,
    /// never its own command.
    bool unit(Fid file);

    /// Whether the build declares `file` a unit: it has a database entry,
    /// or a default-command rule claims it and it is a source.
    bool declared(Fid file);

    /// Whether a file joins the background index: no matching active rule
    /// says `index = false`.
    bool indexed(CanonicalRef path) const;

    /// Whether `clice lint` checks a file: it sits inside the workspace and
    /// no matching active rule says `lint = false`.
    bool lintable(CanonicalRef path) const;

    /// Whether `clice format` formats a file: it sits inside the workspace
    /// and no matching active rule says `format = false`.
    bool formattable(CanonicalRef path) const;

private:
    llvm::SmallVector<const CompiledRule*> matching(CanonicalRef path) const;

    /// The edits of `matched` rules, in declaration order.
    Edits edits_of(llvm::ArrayRef<const CompiledRule*> matched) const;

    /// Whether a file sits inside the workspace and every matching active
    /// rule keeps `field` on: the lint and format sets.
    bool inside(CanonicalRef path, bool CompiledRule::* field) const;

    /// The sources rules declare: the active rules' — and, while the active
    /// configuration declares any, the inactive rules' too, which hide a
    /// registered source instead of leaving it to discovery.
    llvm::SmallVector<SourceID, 4> declared_ids() const;

    /// The first matching active rule declaring a default command.
    const CompiledRule* default_rule(CanonicalRef path) const;

    /// The rule's default command interned; nullopt when it is not a
    /// compile command.
    std::optional<ConfigID> command_of(const CompiledRule& rule);

    /// The interned default command of the first matching active rule
    /// declaring one; nullopt when no rule does or its command is not a
    /// compile command.
    std::optional<ConfigID> default_command(CanonicalRef path);

    /// Whether a default command compiles `path` as a unit: a C-family
    /// source by suffix (never a header), or a file a rule singles out by
    /// pattern whose default command forces a language (`-x c++` for an
    /// extensionless tool). A rule without patterns applies to every file,
    /// so its forced language claims only what clang recognizes — or the
    /// configuration file itself would become a unit.
    bool default_source(CanonicalRef path);

    /// The active rules declaring a default command that can claim a file.
    llvm::SmallVector<const CompiledRule*> claimants() const;

    /// The files of a walk a default-command rule claims: C-family sources
    /// (never headers) matching the rules' patterns.
    void claim_sources(llvm::ArrayRef<CanonicalPath> walked, std::vector<Fid>& out);

    Config& config;
    CompilationDatabase& cdb;
    FileTable& files;
    std::string active;
    std::optional<std::vector<Fid>> claimed_sources;
    llvm::DenseSet<Fid> records;
    std::uint64_t records_generation = 0;
    llvm::DenseMap<Fid, ConfigID> provisional;
};

/// Every file under the walk's roots, version control metadata and the
/// cache directory skipped. File system work only, so it can run off the
/// event loop: a rule without patterns has the whole workspace walked.
std::vector<CanonicalPath> walk_sources(const Build::SourceWalk& walk);

/// The C-family sources and headers of the workspace a refactoring may
/// edit: under `root`, hidden directories, the cache and every build tree
/// (a directory holding CMakeCache.txt or build.ninja) left out.
std::vector<CanonicalPath> workspace_sources(CanonicalRef root, CanonicalRef cache_dir);

/// Whether `file` is the workspace's own — what a refactoring may edit and
/// a save may record as a provisional member: it lies under `root` outside
/// the directories workspace_sources leaves out, whatever its suffix.
bool workspace_file(CanonicalRef root, CanonicalRef cache_dir, CanonicalRef file);

}  // namespace clice
