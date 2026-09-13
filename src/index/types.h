#pragma once

/// The index vocabulary: row and symbol types shared by every layer —
/// builders accumulate them, blob readers hand them out, the project
/// table stores them.

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "semantic/symbol.h"
#include "support/bitmap.h"
#include "syntax/token.h"

#include "llvm/ADT/DenseMap.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

/// Visibility scope of a symbol, determining which level of the multi-level
/// symbol table stores it.
enum class SymbolScope : std::uint8_t {
    /// Can be referenced from any TU (external linkage).  Stored in ProjectIndex.
    External = 0,
    /// Can be referenced across files within one TU but not across TUs
    /// (internal linkage: static, anonymous namespace).  Stored in the main
    /// file's Shard blob.
    TULocal = 1,
    /// Cannot be referenced from any other file (local variables, parameters,
    /// labels).  Stored in the defining file's Shard blob.
    FileLocal = 2,
};

struct Relation {
    /// The raw enum rather than the RelationKind wrapper: the wrapper's
    /// constructors hide it from reflection, and reflection is what lets a
    /// relation vector persist as one contiguous struct vector.
    RelationKind::Kind kind = RelationKind::Invalid;

    std::uint32_t padding = 0;

    LocalSourceRange range;

    SymbolHash target_symbol;

    constexpr void set_definition_range(LocalSourceRange range) {
        target_symbol = std::bit_cast<SymbolHash>(range);
    }

    constexpr auto definition_range() {
        return std::bit_cast<LocalSourceRange>(target_symbol);
    }
};

struct Occurrence {
    Range range;

    /// Hash of the symbol this occurrence names.
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;
};

/// One file's rows while a build accumulates them; encoded into a shard
/// blob (index/shard.h) at build end and consumed as bytes from then on.
struct FileIndex {
    /// The braces matter: fbs decode value-constructs map entries with
    /// `FileIndex{}`, and without an initializer this member would be
    /// copy-initialized from an empty list, which DenseMap's explicit
    /// default constructor rejects.
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations{};

    std::vector<Occurrence> occurrences;

    bool empty() const {
        return occurrences.empty() && relations.empty();
    }
};

/// What the symbol table records about a symbol beyond its name and kind.
/// Each translation unit reports the bits it saw and the project table
/// keeps the union, so a set bit means "some unit saw this".
enum class SymbolFlags : std::uint16_t {
    None = 0,
    /// Some unit holds a definition; without it the symbol is
    /// declaration-only.
    HasDefinition = 1 << 0,
    /// A template pattern, partial specializations included.
    Template = 1 << 1,
    /// An explicit or partial specialization; `Symbol::args` spells its
    /// arguments.
    Specialization = 1 << 2,
    Deprecated = 1 << 3,
    /// An inline namespace: qualified names skip it, as lookup does.
    InlineNamespace = 1 << 4,
    /// No name of its own; `Symbol::name` holds a presentation such as
    /// "(anonymous struct)".
    Unnamed = 1 << 5,
    /// The declaring token comes out of a macro expansion, so no written
    /// source spells the name.
    SpelledInMacro = 1 << 6,
    /// The canonical declaration sits in a system header.
    SystemHeader = 1 << 7,
    /// Offered by unqualified code completion from the index: declared at
    /// namespace scope, or an enumerator of an unscoped enum there or in a
    /// class. Members are completed after `.` by Sema and specializations
    /// share their template's name.
    Completable = 1 << 8,
    /// Three bits holding the NameForm.
    FormMask = 7 << 9,
};

constexpr SymbolFlags operator|(SymbolFlags lhs, SymbolFlags rhs) {
    return static_cast<SymbolFlags>(static_cast<std::uint16_t>(lhs) |
                                    static_cast<std::uint16_t>(rhs));
}

constexpr SymbolFlags& operator|=(SymbolFlags& lhs, SymbolFlags rhs) {
    return lhs = lhs | rhs;
}

constexpr bool has_flag(SymbolFlags flags, SymbolFlags bit) {
    return (static_cast<std::uint16_t>(flags) & static_cast<std::uint16_t>(bit)) != 0;
}

/// The shape of a declaration's name, for consumers that treat special
/// names apart from identifiers (a destructor's `~`, an operator's
/// spelling).
enum class NameForm : std::uint8_t {
    Identifier = 0,
    Constructor,
    Destructor,
    Conversion,
    Operator,
    Literal,
    /// Deduction guides, Objective-C selectors and other names no consumer
    /// tells apart.
    Other,
};

constexpr SymbolFlags with_form(SymbolFlags flags, NameForm form) {
    return static_cast<SymbolFlags>(
        (static_cast<std::uint16_t>(flags) & ~static_cast<std::uint16_t>(SymbolFlags::FormMask)) |
        (static_cast<std::uint16_t>(form) << 9));
}

constexpr NameForm name_form(SymbolFlags flags) {
    return static_cast<NameForm>(
        (static_cast<std::uint16_t>(flags) & static_cast<std::uint16_t>(SymbolFlags::FormMask)) >>
        9);
}

/// No canonical file: the symbol has rows but none of them declares it.
constexpr inline std::uint32_t no_file = ~0u;

struct Symbol {
    /// The symbol's own name: an identifier, or the rendering of a special
    /// name ("~Foo", "operator<<", "operator int"). A presentation for
    /// unnamed entities, marked by SymbolFlags::Unnamed. The qualified
    /// name is the parent chain.
    std::string name;

    /// A specialization's template arguments ("<int, 4>"), shown after the
    /// name; empty otherwise.
    std::string args;

    /// The entity of the enclosing namespace, class, enum or function;
    /// 0 at the translation unit.
    SymbolHash parent = 0;

    SymbolKind kind;

    SymbolScope scope = SymbolScope::External;

    SymbolFlags flags = SymbolFlags::None;

    /// The file holding the canonical declaration — a definition when one
    /// is known, else the first declaration — as a TU-local path id in an
    /// envelope and a FileTable id in the project table; `no_file` when the
    /// symbol is only referenced.
    std::uint32_t file = no_file;

    /// All files that referenced this symbol.
    Bitmap reference_files;

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

using SymbolTable = llvm::DenseMap<SymbolHash, Symbol>;

/// A symbol's identity as a blob reader hands it out; the strings borrow
/// the blob's bytes.
struct SymbolIdentity {
    llvm::StringRef name;
    llvm::StringRef args;
    SymbolHash parent = 0;
    SymbolKind kind;
    SymbolScope scope = SymbolScope::External;
    SymbolFlags flags = SymbolFlags::None;
    std::uint32_t file = no_file;
};

/// A symbol as queries hand it out: its identity plus the stored facts,
/// owned — the strings outlive whichever table answered.
struct SymbolRef {
    SymbolHash hash = 0;
    std::string name;
    std::string args;
    SymbolHash parent = 0;
    SymbolKind kind;
    SymbolFlags flags = SymbolFlags::None;

    /// The name with a specialization's arguments, as display shows it.
    std::string display_name() const {
        return name + args;
    }
};

}  // namespace clice::index
