#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command/command.h"
#include "config/config.h"
#include "vfs/file_table.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A command a file compiles under: one of its database entries
/// (CDBExact) or, for a file without entries that a rule's default command
/// claims, that command (Default).
struct Candidate {
    ConfigID config;
    CommandSource source;
};

/// The rules-applied edits of a set of files: every matching active rule
/// in declaration order, each once, a later remove cancelling an earlier
/// append of the same spelling.
struct Edits {
    std::vector<std::string> append;
    std::vector<std::string> remove;

    CommandOptions options(llvm::ArrayRef<std::string> extra_prepend = {},
                           llvm::ArrayRef<std::string> extra_append = {}) const {
        return {.remove = remove,
                .append = append,
                .extra_prepend = extra_prepend,
                .extra_append = extra_append};
    }
};

/// The build view: which of the database's entries and which hand-written
/// commands apply to a file under the active configuration, and how the
/// rules edit them. The one place that knows rule priority — the database
/// stores entries per source in file order and nothing else consults the
/// rules directly.
class BuildView {
public:
    BuildView(Config& config, CompilationDatabase& cdb, FileTable& files) :
        config(config), cdb(cdb), files(files) {}

    /// The configuration tag every tagged rule is checked against; empty
    /// when the rules declare none.
    llvm::StringRef active_configuration() const {
        return active;
    }

    /// Pick the startup configuration: `default_configuration`, else the
    /// first declared tag.
    void reset_active();

    /// The databases the active view compiles from, in priority order:
    /// the sources of every matching-or-not active rule, deduplicated.
    /// Empty when no rule declares one — discovery's cue.
    llvm::SmallVector<llvm::StringRef> declared_sources() const;

    /// A file's candidate entries in view order: entries from the sources
    /// of rules matching the file first, then from the other active rules'
    /// sources, each in declaration order; discovered sources (registered
    /// without a rule) last; within a source, file order. The first is the
    /// default selection. Entries of sources only inactive rules declare
    /// are excluded.
    llvm::SmallVector<CompilationEntry, 2> candidates(Fid file) const;

    bool has_candidates(Fid file) const {
        return !candidates(file).empty();
    }

    /// The commands a file compiles under, in view order: its candidate
    /// entries, or the default command of the first matching rule that
    /// declares one. The first is the default selection; empty when the
    /// file has neither.
    llvm::SmallVector<Candidate, 2> commands(Fid file);

    /// Whether the view compiles the file at all — the bar for hosting a
    /// header's context.
    bool compiles(Fid file) {
        return !commands(file).empty();
    }

    /// The edits the rules matching any of `paths` contribute, in
    /// declaration order, each rule once — a header borrowing a host's
    /// command passes both so it inherits the host's edits.
    Edits edits(llvm::ArrayRef<llvm::StringRef> paths) const;

    Edits edits(llvm::StringRef path) const {
        return edits(llvm::ArrayRef(path));
    }

    /// The builtin fallback command of a file the view does not compile
    /// (CommandSource::Fallback): the driver follows the language clang
    /// assigns to the file's extension.
    ConfigID builtin(llvm::StringRef path);

    /// Apply the edits of `paths` (and a run's extras) to `base`: the
    /// effective command of `file`, whose language is derived from the
    /// edited command and `language_path` (the file itself, or the host a
    /// header borrows from).
    CommandRef resolve(Fid file,
                       ConfigID base,
                       CommandSource source,
                       llvm::ArrayRef<llvm::StringRef> paths,
                       llvm::StringRef language_path,
                       llvm::ArrayRef<std::string> extra_prepend = {},
                       llvm::ArrayRef<std::string> extra_append = {});

    /// Hash of the edits for `paths`, empty when none apply: the rules'
    /// part of a file's persisted command identity.
    std::string edit_hash(llvm::ArrayRef<llvm::StringRef> paths) const;

    /// Every translation unit of the active view: files with candidate
    /// entries, plus the source files on disk that a default-command rule
    /// matches.
    std::vector<Fid> members() const;

    /// Whether a file joins the background index: no matching active rule
    /// says `index = false`.
    bool indexed(llvm::StringRef path) const;

private:
    llvm::SmallVector<const CompiledRule*> matching(llvm::StringRef path) const;

    /// The interned default command of the first matching active rule
    /// declaring one; nullopt when no rule does or its command is not a
    /// compile command.
    std::optional<ConfigID> default_command(llvm::StringRef path);

    /// Files a default-command rule claims: C-family sources (never
    /// headers) under the rules' pattern roots matching their patterns,
    /// skipping the cache directory and version control metadata.
    void enumerate_default_sources(std::vector<Fid>& out) const;

    Config& config;
    CompilationDatabase& cdb;
    FileTable& files;
    std::string active;
};

}  // namespace clice
