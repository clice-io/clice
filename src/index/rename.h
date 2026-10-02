#pragma once

/// Rename: the symbols one name change touches, the tokens spelling their
/// name, and what keeps the change from being safe. The index answers all
/// of it — no recompile: a rename is the rows of a symbol group, each
/// checked against the text it indexed, plus a sweep of the workspace for
/// the tokens spelling the old name that the index ties to nothing.

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "index/query.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice::index {

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
    IndexQuery::Located symbol;
    std::vector<IndexQuery::Located> group;
};

/// The rename target of a located symbol; the error says why the symbol
/// cannot be renamed (an operator, a macro, a name no source spells).
std::expected<RenameTarget, std::string> rename_target(const IndexQuery& query,
                                                       const IndexQuery::Located& named);

/// The rename an editor starts at a cursor: the target of the symbols
/// there, and the token at the cursor spelling its name.
struct CursorRename {
    RenameTarget target;
    Site token;
};

/// The rename at a cursor. Its symbols must agree on one target (a class
/// and the constructor a construction calls do); the error says why they
/// do not, or why the token there cannot be renamed — it spells no name
/// of the target, or its file moved on since it was indexed.
std::expected<CursorRename, std::string> rename_at(const IndexQuery& query,
                                                   const IndexQuery::Cursor& cursor);

/// Where a rename may change text, and how to read it.
struct RenameScope {
    /// The workspace files a rename may edit and searches for the old name,
    /// absolute: its sources and headers, build and cache directories left
    /// out. An edit outside them is a conflict.
    llvm::ArrayRef<std::string> files;

    /// The current text of a file: an open buffer's, else the disk's.
    llvm::function_ref<std::optional<std::string>(llvm::StringRef path)> read;

    /// Whether some unit of the build the index is to hold has no record
    /// in it yet: a file the index holds no rows of may then be one the
    /// unit compiles or includes, unindexed rather than outside the index.
    bool units_pending = false;
};

/// One token the rename replaces.
struct RenameEdit {
    Site site;

    /// Reached through a name the index resolved heuristically (a
    /// dependent call, a using-declaration of an overload set): changed,
    /// and worth a look.
    bool heuristic = false;
};

/// A token spelling the old name that the rename leaves alone, and why.
struct RenameNote {
    Site site;
    std::string reason;

    /// The source line it stands on, trimmed.
    std::string line;
};

struct RenamePlan {
    std::string old_name;

    /// Sorted by file, then offset.
    std::vector<RenameEdit> edits;

    std::vector<RenameNote> unconfirmed;

    /// What makes the rename unsafe; nothing is written while any stands.
    std::vector<std::string> conflicts;

    std::vector<std::string> warnings;

    /// Files spelling the old name whose rows are not current — their text
    /// moved on, or the build compiles them but the index never did.
    /// Nothing is written while any stands.
    std::vector<std::string> stale;

    bool blocked() const {
        return !conflicts.empty() || !stale.empty();
    }
};

RenamePlan plan_rename(const IndexQuery& query,
                       FileTable& files,
                       const RenameTarget& target,
                       llvm::StringRef new_name,
                       const RenameScope& scope);

/// `text` with the plan's edits of `file` applied: each old-name token
/// replaced by `new_name`. Nullopt when a token no longer spells the old
/// name — the text moved on since the plan was made.
std::optional<std::string>
    apply_rename(llvm::StringRef text, const RenamePlan& plan, Fid file, llvm::StringRef new_name);

}  // namespace clice::index
