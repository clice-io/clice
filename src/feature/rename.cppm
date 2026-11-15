/// Rename: the symbols one name change touches, the tokens spelling their
/// name, and what keeps the change from being safe. The index answers all
/// of it — no recompile, and no read of the project's other files: a
/// rename is the entries of a symbol group, each checked against the text
/// it indexed. What the index ties to no symbol — the code an `#if` leaves
/// out, a `#define` body no expansion runs — is not renamed, nor listed.

module clice:feature.rename;

import :index.query;
import :support.prelude;
import :syntax.position;

namespace clice::feature {

/// Why `name` cannot name a C or C++ declaration — not an identifier, or a
/// keyword in either language; nullopt when it can.
std::optional<std::string> invalid_identifier(llvm::StringRef name);

/// The symbols one rename changes: the symbol named, after a constructor,
/// destructor, deduction guide or specialization resolved to its class or
/// template, and every symbol spelling the same name with it — its
/// constructors, destructor and deduction guides, its specializations and
/// theirs, the overrides a virtual function is linked with. A macro's are
/// the definitions of its name linked with it: those in a file defining
/// or using one of them (the branches of an `#if`, a redefinition after
/// `#undef`), and those a use of one resolves to as well (a shared header
/// under other definitions, the expansions of a body naming it).
struct RenameTarget {
    index::IndexQuery::Located symbol;
    std::vector<index::IndexQuery::Located> group;
};

/// The rename target of a located symbol; the error says why the symbol
/// cannot be renamed (an operator, a name no source spells, a macro the
/// command line defines).
std::expected<RenameTarget, std::string> rename_target(const index::IndexQuery& query,
                                                       const index::IndexQuery::Located& named);

/// The rename an editor starts at a cursor: the target of the symbols
/// there, and the token at the cursor spelling its name.
struct CursorRename {
    RenameTarget target;
    index::Site token;
};

/// The rename at a cursor. Its symbols must agree on one target (a class
/// and the constructor a construction calls do); the error says why they
/// do not, or why the token there cannot be renamed — it spells no name
/// of the target, or its file moved on since it was indexed.
std::expected<CursorRename, std::string> rename_at(const index::IndexQuery& query,
                                                   const index::IndexQuery::Cursor& cursor);

/// Where a rename may change text, and how to read it.
struct RenameScope {
    /// Whether the rename may edit a file: one in the project outside
    /// its build trees, hidden directories and cache directory, whatever
    /// its suffix; from a file outside every workspace folder, that file
    /// alone. An edit elsewhere refuses the rename.
    llvm::function_ref<bool(llvm::StringRef path)> editable;

    /// A file's current text — an open buffer's, else the disk's; nullopt
    /// when it cannot be read. Read only for the files the rename edits
    /// and those the index holds whose text moved on since.
    llvm::function_ref<std::optional<std::string>(llvm::StringRef path)> read;

    /// Whether some unit of the build the index is to hold has no record
    /// in it yet: what it names goes unrenamed, which a warning says.
    bool units_pending = false;

    /// The rename starts in a file outside every workspace folder: what it
    /// may not edit lies in no folder, rather than outside the project's
    /// sources.
    bool outside_folders = false;
};

/// One token the rename replaces.
struct RenameEdit {
    index::Site site;
};

/// A token spelling the old name that the rename leaves alone, and why.
struct RenameNote {
    index::Site site;
    std::string reason;

    /// The source line it stands on, trimmed.
    std::string line;
};

struct RenamePlan {
    std::string old_name;
    std::string new_name;

    /// Sorted by file, then offset.
    std::vector<RenameEdit> edits;

    std::vector<RenameNote> unconfirmed;

    /// What makes the rename unsafe; nothing is written while any stands.
    std::vector<std::string> conflicts;

    /// Why the rename may not change the files it would, one reason each:
    /// they are not the scope's to edit. Nothing is written while any
    /// stands.
    std::vector<std::string> outside;

    std::vector<std::string> warnings;

    /// Files the rename edits, or that spell its old or new name, whose
    /// text moved on since they were indexed. Nothing is written while any
    /// stands.
    std::vector<std::string> stale;

    /// Files the rename edits that the index holds no shard of yet: an open
    /// file's compile named them before the background index reached them.
    /// Nothing is written while any stands.
    std::vector<std::string> unindexed;

    bool blocked() const {
        return !conflicts.empty() || !outside.empty() || !stale.empty() || !unindexed.empty();
    }
};

/// What the other renames of a batch do to names: the symbols each gives
/// a new name, and the symbols ending with each new name. A rename judges
/// its new name by the names the whole batch leaves; alone, by today's.
struct RenameBatch {
    llvm::DenseMap<index::SymbolHash, std::string> renamed;
    llvm::StringMap<std::vector<index::IndexQuery::Located>> named;
};

RenamePlan plan_rename(const index::IndexQuery& query,
                       FileTable& files,
                       const RenameTarget& target,
                       llvm::StringRef new_name,
                       const RenameScope& scope,
                       const RenameBatch& batch = RenameBatch());

/// Why a rename of `target` cannot be made whatever its new name: it
/// would change files `scope` does not let it edit (RenamePlan::outside);
/// nullopt when it may edit every file it changes.
std::optional<std::string> outside_refusal(const index::IndexQuery& query,
                                           FileTable& files,
                                           const RenameTarget& target,
                                           const RenameScope& scope);

/// One rename of a batch: the symbols it changes and the name they get.
struct Renaming {
    RenameTarget target;
    std::string new_name;
};

/// A token a batch replaces, and the plan replacing it.
struct BatchEdit {
    RenameEdit edit;
    std::uint32_t plan;
};

/// Renames planned together against one index and one reading of the
/// project, all applying at once: `A` to `B` beside `B` to `C` renames
/// the old `B`, and two names may swap.
struct BatchPlan {
    /// One per renaming, in order.
    std::vector<RenamePlan> plans;

    /// Every plan's edits, sorted by file, then offset, each token once.
    std::vector<BatchEdit> edits;

    /// What makes the batch unsafe beyond its plans' own conflicts: a
    /// token two renamings would change to different names.
    std::vector<std::string> conflicts;

    bool blocked() const {
        return !conflicts.empty() || ranges::any_of(plans, &RenamePlan::blocked);
    }
};

/// Plan `renamings` together, each file whose index moved on read once
/// for the old and new names of all of them.
BatchPlan plan_renames(const index::IndexQuery& query,
                       FileTable& files,
                       llvm::ArrayRef<Renaming> renamings,
                       const RenameScope& scope);

/// `text` with the batch's edits of `file` applied, each token replaced
/// by the new name of its plan. Nullopt when a token no longer spells its
/// old name — the text moved on since the plan was made.
std::optional<std::string> apply_renames(llvm::StringRef text, const BatchPlan& batch, Fid file);

/// Why a blocked plan (RenamePlan::blocked) cannot be made: its conflicts,
/// the files it may not edit, then the files whose index is not current.
std::string refusal(const RenamePlan& plan);

/// The edits of a rename, each file's versioned against its open buffer,
/// and what the user should hear about them: the warnings and the tokens
/// spelling the old name that were left alone.
struct Renamed {
    protocol::WorkspaceEdit edit;
    std::string notice;
};

/// A plan's edits renaming to `new_name`, each file versioned by
/// `version_of` (none for a file not open).
Renamed to_protocol(const RenamePlan& plan,
                    llvm::StringRef new_name,
                    llvm::function_ref<std::optional<int>(Fid)> version_of,
                    PositionEncoding encoding);

}  // namespace clice::feature
