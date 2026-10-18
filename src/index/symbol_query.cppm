module;

#include "modules/prelude.h"

/// The one grammar every symbol lookup speaks: the locators the clice
/// commands take, and workspace/symbol.
///
///   foo                  fuzzy; the exact name ranks first, then prefixes
///   "foo"                the whole name, case-sensitive
///   foo*  *_test  get?X  glob; case-sensitive once the pattern has an uppercase letter
///   ns::Foo::bar         inside a container whose chain lists these, in order
///   ::ns::Foo::bar       inside exactly that container
///   ns::*   ns::**       the container's members; its whole subtree
///   Widget<int>          a specialization, by its arguments
///   #1a2b3c              the symbol id an earlier answer carried, with `path:`
///                        naming the file of a symbol only some files see
///   src/a.cpp:120        the symbols defined on that line
///   src/a.cpp:120:8      the symbol under that cursor (1-based line and byte column)
///   kind:function        of these kinds (comma-separated, or the term repeated)
///   path:src/index/      declared in that file or under that directory, a
///                        relative path from the workspace root; a bare file
///                        name matches by name
///
/// Terms are separated by whitespace; quotes and angle brackets keep
/// theirs. One term names the symbol, the rest narrow it.

module clice:index.symbol_query;

import :index.symbol;
import :semantic.symbol;
import :vfs.path;

namespace clice::index {

struct SymbolQuery {
    enum class Mode : std::uint8_t {
        /// The pattern as a subsequence, ranked by score.
        Fuzzy,
        /// The pattern as the whole name.
        Exact,
        /// The pattern with `*` and `?` wildcards.
        Glob,
        /// The scope's direct members, whatever their name.
        Members,
        /// Everything below the scope.
        Subtree,
    };

    /// One name of a container chain, with the arguments a specialization
    /// must spell (empty for any).
    struct Segment {
        std::string name;
        std::string args;
    };

    /// A place in a file.
    struct Position {
        std::string path;
        int line = 0;
        std::optional<int> column;
    };

    Mode mode = Mode::Fuzzy;

    /// The containers the symbol lies in, outermost first.
    std::vector<Segment> scope;

    /// Whether `scope` is the whole chain from the translation unit.
    bool absolute = false;

    /// The name pattern under `mode`; empty under Fuzzy lists everything.
    std::string pattern;

    /// The arguments a specialization must spell, empty for any.
    std::string args;

    std::vector<SymbolKind> kinds;

    /// Path filters, each a bare file name or a path, absolute once
    /// anchored; with a handle, the file whose entries anchor it — a symbol
    /// only some files see has no project-wide entry.
    std::vector<std::string> paths;

    /// Leave out the names of internal linkage, which only their own unit
    /// can name.
    bool external_only = false;

    /// Set instead of a pattern: the symbol with this id.
    std::optional<SymbolHash> handle;

    /// Set instead of a pattern: the symbol at this place.
    std::optional<Position> position;

    /// Parse a query; the error names what could not be read.
    static std::expected<SymbolQuery, std::string> parse(llvm::StringRef text);

    /// Take the path filters spelling a path as identities, relative ones
    /// from the workspace `root`; a bare file name stays a name.
    void resolve_paths(const Spelling& root);

    /// Whether the query names symbols by pattern rather than by id or
    /// place.
    bool by_pattern() const {
        return !handle && !position;
    }

    /// Whether the symbol must sit directly in the scope's container
    /// rather than anywhere below it: a members listing, and any name
    /// lookup under an absolute chain.
    bool direct() const {
        return mode == Mode::Members || (mode != Mode::Subtree && absolute);
    }
};

/// A symbol's id on the command line and in JSON answers: `#<hex>`, which
/// a JavaScript number would round.
inline std::string symbol_id(SymbolHash hash) {
    return std::format("#{:016x}", hash);
}

/// The locator naming a symbol back: its `#<hex>` id, followed by `path:`
/// and `anchor` when there is one — for a symbol only some files see, a
/// file holding its entries.
std::string symbol_locator(SymbolHash hash, llvm::StringRef anchor);

/// Whether a symbol's arguments satisfy the query's: any when the query
/// spells none, else the same modulo whitespace.
bool args_match(llvm::StringRef wanted, llvm::StringRef actual);

/// Whether `name` matches a glob pattern, case-insensitively unless the
/// pattern has an uppercase letter.
bool glob_matches(llvm::StringRef pattern, llvm::StringRef name);

/// The literal runs between a glob pattern's wildcards.
llvm::SmallVector<llvm::StringRef, 4> glob_literals(llvm::StringRef pattern);

/// Whether `file` satisfies an anchored `path:` term (SymbolQuery::resolve_paths):
/// a bare file name by name, a path by being that file or lying under that
/// directory.
bool path_matches(llvm::StringRef wanted, llvm::StringRef file);

/// One container of a symbol's chain as the scope check sees it.
struct ScopeEntry {
    llvm::StringRef name;
    llvm::StringRef args;
};

/// Whether scope segments name a container chain (outermost first):
/// entry for entry when `exact`, else in order with gaps allowed. Names
/// compare case-insensitively, arguments through args_match.
bool segments_match(llvm::ArrayRef<SymbolQuery::Segment> segments,
                    llvm::ArrayRef<ScopeEntry> chain,
                    bool exact);

/// Whether a symbol whose containers are `chain` (outermost first, inline
/// namespaces left out) lies where the query looks: in exactly the
/// scope's chain when absolute, else anywhere below a container the
/// scope's segments lead to in order — or, for a members listing,
/// directly in it.
bool in_scope(const SymbolQuery& query, llvm::ArrayRef<ScopeEntry> chain);

}  // namespace clice::index
