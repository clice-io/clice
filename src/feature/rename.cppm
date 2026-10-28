module;

#include "support/prelude.h"

/// Rename: the symbols one name change touches, the tokens spelling their
/// name, and what keeps the change from being safe. The index answers all
/// of it — no recompile: a rename is the entries of a symbol group, each
/// checked against the text it indexed, plus a scan of the workspace for
/// the tokens spelling the old name that the index ties to nothing.

module clice:feature.rename;

import :index.query;
import :syntax.position;

namespace clice::feature {

/// Why `name` cannot name a C or C++ declaration — not an identifier, or a
/// keyword in either language; nullopt when it can.
std::optional<std::string> invalid_identifier(llvm::StringRef name);

/// The symbols one rename changes: the symbol named, after a constructor,
/// destructor, deduction guide or specialization resolved to its class or
/// template, and every symbol spelling the same name with it — its
/// constructors and destructor, its specializations and theirs, the
/// overrides a virtual function is linked with. A class template's
/// deduction guides join in plan_rename.
struct RenameTarget {
    index::IndexQuery::Located symbol;
    std::vector<index::IndexQuery::Located> group;
};

/// The rename target of a located symbol; the error says why the symbol
/// cannot be renamed (an operator, a macro, a name no source spells).
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

/// A file's current text as a rename's scan looks at it: the tokens
/// spelling each name scanned — a rename's old and new name, or those of
/// every rename of a batch.
struct ScannedText {
    std::string text;

    struct Token {
        std::uint32_t offset;
        /// It stands in a preprocessor directive.
        bool directive;
    };

    llvm::StringMap<std::vector<Token>> tokens;
};

/// Scan `text` for `names`; nullopt when it spells none of them as a
/// token. Text work alone, safe on any thread: a server runs it off its
/// event loop over the whole workspace.
std::optional<ScannedText> scan_text(std::string text, const llvm::StringSet<>& names);

/// The texts a rename reads (RenameScope::read): the workspace's files
/// scanned for its names up front, and on first read the text of an open
/// buffer or of an edited file the walk's suffixes left out.
struct WorkspaceTexts {
    /// Read and scan `files`, the workspace's sources and headers by
    /// suffix (RenameScope::files): file work alone, safe on any thread.
    WorkspaceTexts(std::vector<std::string> files, llvm::StringSet<> names);

    /// A file's text scanned for the names: the open buffer `buffer`
    /// gives, else the disk's; null when it spells none.
    const ScannedText*
        read(llvm::StringRef path,
             llvm::function_ref<std::optional<std::string>(llvm::StringRef path)> buffer = nullptr);

    std::vector<std::string> files;

private:
    const ScannedText* read_late(llvm::StringRef path, std::optional<std::string> text);

private:
    llvm::StringSet<> names;

    llvm::StringSet<> walked;

    /// The walked files spelling a name.
    llvm::StringMap<ScannedText> scanned;

    /// The texts read on demand: buffers, and files the walk left out.
    llvm::StringMap<std::optional<ScannedText>> late;
};

/// Why no rename can be made in the workspace rooted at `root`: one
/// without a folder has no sources to edit; nullopt when one can.
std::optional<std::string> workspace_refusal(llvm::StringRef root);

/// Where a rename may change text, and how to read it.
struct RenameScope {
    /// The workspace files the scan looks at, absolute: its sources and
    /// headers by suffix, build trees, hidden directories and the cache
    /// directory left out.
    llvm::ArrayRef<std::string> files;

    /// Whether the rename may edit a file: one in the workspace outside
    /// those directories, whatever its suffix. An edit elsewhere is a
    /// conflict.
    llvm::function_ref<bool(llvm::StringRef path)> editable;

    /// A file's current text — an open buffer's, else the disk's — scanned
    /// for the names of the rename (scan_text); null when it spells none.
    llvm::function_ref<const ScannedText*(llvm::StringRef path)> read;

    /// Whether some unit of the build the index is to hold has no record
    /// in it yet: a file the index holds no shard of may then be one the
    /// unit compiles or includes, unindexed rather than outside the index.
    bool units_pending = false;
};

/// One token the rename replaces.
struct RenameEdit {
    index::Site site;

    /// Reached through a name the index resolved heuristically (a
    /// dependent call, a using-declaration of an overload set): changed,
    /// and worth a look.
    bool heuristic = false;
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

    std::vector<std::string> warnings;

    /// Files the rename edits, or that spell its old or new name, whose
    /// index is not current — their text moved on, or the build compiles
    /// them but the index never did. Nothing is written while any stands.
    std::vector<std::string> stale;

    bool blocked() const {
        return !conflicts.empty() || !stale.empty();
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
/// workspace, all applying at once: `A` to `B` beside `B` to `C` renames
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

/// Plan `renamings` together. `scope.read` scans a file for the old and
/// new names of all of them; each plan looks only at the files spelling
/// its own two.
BatchPlan plan_renames(const index::IndexQuery& query,
                       FileTable& files,
                       llvm::ArrayRef<Renaming> renamings,
                       const RenameScope& scope);

/// `text` with the batch's edits of `file` applied, each token replaced
/// by the new name of its plan. Nullopt when a token no longer spells its
/// old name — the text moved on since the plan was made.
std::optional<std::string> apply_renames(llvm::StringRef text, const BatchPlan& batch, Fid file);

/// Why a blocked plan (RenamePlan::blocked) cannot be made: its conflicts,
/// then the files whose index is not current.
std::string refusal(const RenamePlan& plan);

/// The edits of a rename, each file's versioned against its open buffer,
/// and what the user should hear about them: the warnings, the tokens
/// spelling the old name that were left alone, the edits reached through a
/// heuristic resolution.
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
