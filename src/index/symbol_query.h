#pragma once

/// The one grammar every symbol lookup speaks: `clice query`'s --query
/// and --name, workspace/symbol, and the locators of the questions about
/// one symbol.
///
///   foo                  fuzzy; the exact name ranks first, then prefixes
///   "foo"                the whole name, case-sensitive
///   foo*  *_test  get?X  glob; case-sensitive once the pattern has an uppercase letter
///   ns::Foo::bar         inside a container whose chain lists these, in order
///   ::ns::Foo::bar       inside exactly that container
///   ns::*   ns::**       the container's members; its whole subtree
///   Widget<int>          a specialization, by its arguments
///   #1a2b3c              the symbol id an earlier answer carried
///   src/a.cpp:120        the symbols defined on that line
///   src/a.cpp:120:8      the symbol under that cursor (1-based line and byte column)
///   kind:function        of these kinds (comma-separated, or the term repeated)
///   path:src/index/      declared under that directory; a bare file name
///                        matches by name, any other path by its tail
///
/// Terms are separated by whitespace; quotes and angle brackets keep
/// theirs. One term names the symbol, the rest narrow it.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "index/types.h"
#include "semantic/symbol.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

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

    std::vector<std::string> paths;

    /// Set instead of a pattern: the symbol with this id.
    std::optional<SymbolHash> handle;

    /// Set instead of a pattern: the symbol at this place.
    std::optional<Position> position;

    /// Parse a query; the error names what could not be read.
    static std::expected<SymbolQuery, std::string> parse(llvm::StringRef text);

    /// The symbol kind a `kind:` term names, case-insensitively.
    static std::optional<SymbolKind> parse_kind(llvm::StringRef name);

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

/// Whether a symbol's arguments satisfy the query's: any when the query
/// spells none, else the same modulo whitespace.
bool args_match(llvm::StringRef wanted, llvm::StringRef actual);

/// Whether `name` matches a glob pattern, case-insensitively unless the
/// pattern has an uppercase letter.
bool glob_matches(llvm::StringRef pattern, llvm::StringRef name);

/// The literal runs between a glob pattern's wildcards.
llvm::SmallVector<llvm::StringRef, 4> glob_literals(llvm::StringRef pattern);

/// Whether a file satisfies a `path:` term: a bare file name by name, a
/// directory (trailing separator) by containing it, an absolute path
/// exactly (or as a directory prefix with its separator), any other path
/// by its tail on a component boundary.
bool path_matches(llvm::StringRef wanted, llvm::StringRef path);

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
