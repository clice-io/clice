module clice:settings.rules;

import :command.edit;
import :index.database;
import :settings.settings;
import :support.prelude;
import :vfs.path;

namespace clice {

/// The rules-applied edits of a set of files: every matching active rule
/// in declaration order, each once, its removes before its appends; the
/// toolchain query off when any of them turns it off.
struct Edits {
    std::vector<CommandEdit> edits;
    bool query_toolchain = true;

    bool empty() const {
        return edits.empty() && query_toolchain;
    }

    CommandEdits options(llvm::ArrayRef<std::string> extra_prepend = {},
                         llvm::ArrayRef<std::string> extra_append = {}) const {
        return {.edits = edits,
                .extra_prepend = extra_prepend,
                .extra_append = extra_append,
                .query_toolchain = query_toolchain};
    }
};

/// The directories the default-command rules cover sources under, none
/// inside another, and the cache directory a walk of them skips.
struct SourceWalk {
    llvm::SmallVector<CanonicalPath> roots;
    /// The roots of rules with patterns: a file there may be covered
    /// whatever its suffix (a forced language), elsewhere only one
    /// clang recognizes.
    llvm::SmallVector<CanonicalPath> patterned;
    CanonicalPath cache_dir;
};

/// The rules of clice.toml under one configuration: which a file matches,
/// how they edit its command, whether it is indexed, linted and formatted,
/// which default command covers it, which databases it compiles from in
/// what order, and which index database the configuration's build reads —
/// the one place that knows rule priority. A view of the settings it was
/// made over, which outlive it unchanged.
class Rules {
public:
    /// The rules of `settings` under `configuration` — a declared tag, or
    /// empty when the rules declare none (see choose_configuration) — for
    /// the project at `root`.
    Rules(const Settings& settings, CanonicalRef root, llvm::StringRef configuration);

    /// The configuration tag every tagged rule is checked against; empty
    /// when the rules declare none.
    llvm::StringRef configuration() const {
        return tag;
    }

    /// The active rules matching `path`, in declaration order.
    llvm::SmallVector<const CompiledRule*> matching(CanonicalRef path) const;

    /// Whether an active rule declares a command source — a database or a
    /// default command. Declared sources turn automatic discovery off.
    bool declares_sources() const {
        return declares;
    }

    /// The databases the active rules declare, in declaration order, each
    /// once. Empty when no rule declares one — discovery's cue.
    llvm::SmallVector<Spelling> declared_databases() const;

    /// The databases `path` compiles from as the rules declare them, in
    /// priority order: those of the active rules matching it, then those of
    /// the other active rules, each in declaration order.
    llvm::SmallVector<const Spelling*> database_order(CanonicalRef path) const;

    /// The databases discovery leaves to the rules: those of the active
    /// rules — and, while they declare sources, of the inactive ones too,
    /// which hide a registered database instead of leaving it to discovery.
    llvm::SmallVector<const Spelling*> hiding_databases() const;

    /// The edits the rules matching any of `paths` contribute, in
    /// declaration order, each rule once.
    Edits edits(llvm::ArrayRef<CanonicalRef> paths) const;

    /// The identity of `edits` as a command identity records it
    /// (Project::command_identity), the project root taken as
    /// `${workspace}` so that a moved checkout keeps it; 0 for none.
    std::uint64_t identity(const Edits& edits) const;

    /// The first matching active rule declaring a default command.
    const CompiledRule* default_rule(CanonicalRef path) const;

    /// Whether a file joins the background index: no matching active rule
    /// says `index = false`.
    bool indexable(CanonicalRef path) const;

    /// Whether `clice lint` checks a file: it sits inside the project root and
    /// no matching active rule says `lint = false`.
    bool lintable(CanonicalRef path) const;

    /// Whether `clice format` formats a file: it sits inside the project root
    /// and no matching active rule says `format = false`.
    bool formattable(CanonicalRef path) const;

    /// The active rules declaring a default command that can cover a file.
    llvm::SmallVector<const CompiledRule*> covering() const;

    /// What a default-sources enumeration walks; no roots when no active
    /// rule covers sources.
    SourceWalk source_walk() const;

    /// The real paths that the databases of the active rules resolving
    /// symlinks (Rule::resolve_symlinks) resolve to now, one per line; empty
    /// when no active rule resolves them.
    std::string resolved_databases() const;

    /// The index database the build reads, its databases `resolved`
    /// (resolved_databases): the configuration's own when nothing resolved;
    /// else the one of those real paths, or the own one of another
    /// configuration whose rules differ from these only in naming exactly
    /// those databases directly.
    index::DatabaseKey database_key(llvm::StringRef resolved) const;

private:
    /// Whether a file sits inside the project root, when there is one, and every
    /// matching active rule keeps `field` on: the lint and format sets.
    bool inside(CanonicalRef path, bool CompiledRule::* field) const;

    const Settings* settings;

    CanonicalPath root;

    std::string tag;

    /// The rules active under the configuration, in declaration order.
    llvm::SmallVector<const CompiledRule*> active;

    bool declares = false;
};

}  // namespace clice
