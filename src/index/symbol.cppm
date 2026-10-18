module;

#include "modules/prelude.h"

/// The index vocabulary: entry and symbol types shared by every layer —
/// builders accumulate them, blob readers hand them out, the project
/// table stores them.

module clice:index.symbol;

import :semantic.symbol;
import :semantic.token_class;
import :support.bitmap;
import :syntax.token;

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

/// Identity of one variant of a file's index: xxh3_64 over the encoded
/// single-variant blob bytes the worker produced. Encoding is canonical
/// and deterministic, so two compilation contexts whose preprocessing of
/// the file agrees produce byte-identical blobs and share one identity.
using VariantHash = std::uint64_t;

/// The identity of one resolution — the records one instantiated definition or
/// macro expansion resolves in one file (resolution_hash).
using ResolutionHash = std::uint64_t;

/// The identity of the resolutions a TU uses in one file (use_hash). TUs
/// using the same ones share it, and the file's blob stores them once.
using UseHash = std::uint64_t;

/// The parent of a node whose directive sits in the TU's own file, and
/// node_of's answer for a file entered without a directive: the TU root is
/// not itself a node.
constexpr inline std::uint32_t no_node = ~0u;

/// One include directive of an include tree: which file it names, in
/// which file it sits (the parent node's file at `line`), and whether the
/// preprocessor entered the file there. Multiple entries of one file
/// (headers without guards) are distinct nodes; so is a directive whose
/// target was skipped (guarded or `#pragma once`, entered earlier) — an
/// edge of the document with no file entry behind it. The same nodes
/// travel in the TU index and persist in the manifest; only `file`
/// changes meaning at merge: a TU-local path id in the TU index, a
/// FileVersion id (VersionID::raw) in a manifest.
struct IncludeNode {
    std::uint32_t file = 0;

    /// Index of the including node.
    std::uint32_t parent = no_node;

    /// 1-based line of the include directive in the parent.
    std::uint32_t line = 0;

    /// The directive did not enter the file: nothing hangs off this node
    /// and node_of never answers it.
    bool skipped = false;

    friend bool operator==(const IncludeNode&, const IncludeNode&) = default;
};

/// Visibility scope of a symbol, determining which level of the multi-level
/// symbol table stores it.
enum class SymbolScope : std::uint8_t {
    /// Can be referenced from any TU (external or module linkage). Stored
    /// in ProjectIndex, whose reference bitmaps list the files holding its
    /// entries.
    External = 0,
    /// Can be referenced across files within one TU but not across TUs
    /// (internal linkage: static, anonymous namespace). Stored in the
    /// shards holding its entries; the TUs that reference it from several
    /// files list those files (TUManifest::shared_locals).
    Internal = 1,
    /// Declared inside a function (local variables, parameters, labels,
    /// local classes). Stored in the defining file's Shard blob; one another
    /// file names — through an instantiation, a macro expansion, or a local
    /// class a function returns — is listed as for Internal.
    FunctionLocal = 2,
};

struct Relation {
    /// The raw enum rather than the RelationKind wrapper: the wrapper's
    /// constructors hide it from reflection, and reflection is what lets a
    /// relation vector persist as one contiguous struct vector.
    RelationKind::Kind kind = RelationKind::Invalid;

    std::uint32_t padding = 0;

    LocalSourceRange range;

    SymbolHash target;

    constexpr void set_extent(LocalSourceRange range) {
        target = std::bit_cast<SymbolHash>(range);
    }

    constexpr auto extent() const {
        return std::bit_cast<LocalSourceRange>(target);
    }
};

struct Occurrence {
    Range range;

    /// Hash of the symbol this occurrence names.
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;
};

/// One file's index while a build accumulates it; encoded into a shard
/// blob (index/shard.cppm) at build end and consumed as bytes from then on.
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
    /// Every declaration's name token comes out of a macro expansion, so no
    /// written source spells the name.
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
    /// Exported by its module (decls::is_exported).
    Exported = 1 << 12,
    /// An anonymous struct or union, or an unscoped enum without a name:
    /// the enclosing scope names its members.
    AnonymousScope = 1 << 13,
    /// Declared in an anonymous namespace inside the scope its qualified
    /// name starts from, which the parent chain skips.
    AnonymousNamespace = 1 << 14,
    /// A template parameter.
    TemplateParameter = 1 << 15,
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

/// Whether a qualified name skips this container, as lookup does.
constexpr bool transparent_scope(SymbolFlags flags) {
    return has_flag(flags, SymbolFlags::InlineNamespace) ||
           has_flag(flags, SymbolFlags::AnonymousScope);
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

/// No canonical file: the symbol has entries but none of them declares it.
constexpr inline std::uint32_t no_file = ~0u;

/// A symbol's identity as a blob reader hands it out; the strings borrow
/// the blob's bytes.
struct SymbolView {
    llvm::StringRef name;
    llvm::StringRef args;
    llvm::StringRef signature;
    SymbolHash parent = 0;
    SymbolKind kind;
    SymbolScope scope = SymbolScope::External;
    SymbolFlags flags = SymbolFlags::None;
    std::uint32_t file = no_file;
    TokenClass token_class;
};

struct Symbol {
    /// The symbol's own name: an identifier, or the rendering of a special
    /// name ("~Foo", "operator<<", "operator int"). A presentation for
    /// unnamed entities, marked by SymbolFlags::Unnamed. The qualified
    /// name is the parent chain.
    std::string name;

    /// A specialization's template arguments ("<int, 4>"), shown after the
    /// name; empty otherwise.
    std::string args;

    /// A function's parameters and qualifiers as every feature shows them
    /// (display::call_signature), which tell its overloads apart; empty
    /// for any other symbol.
    std::string signature;

    /// The entity of the enclosing namespace, class, enum or function;
    /// 0 at the translation unit.
    SymbolHash parent = 0;

    SymbolKind kind;

    SymbolScope scope = SymbolScope::External;

    SymbolFlags flags = SymbolFlags::None;

    /// What the symbol's written names show as semantic tokens wherever
    /// they occur (token_class).
    TokenClass token_class;

    /// The file holding the canonical declaration — a definition when one
    /// is known, else the first declaration — as a TU-local path id in a
    /// TU index and a FileTable id in the project table; `no_file` when the
    /// symbol is only referenced.
    std::uint32_t file = no_file;

    /// All files that referenced this symbol; for an internal-linkage one
    /// also the files whose relations target it (a structured binding's
    /// TypeDefinition relation naming a type the file never spells), and
    /// the ones whose instantiations name it.
    Bitmap reference_files;

    /// For an external symbol, the files whose instantiation shards name
    /// it, which the questions about its references, calls, definitions
    /// and type hierarchy read, not rename or the name search.
    Bitmap instantiation_files;

    /// The identity a reader hands out for this entry; the strings borrow it.
    SymbolView view() const {
        return {
            .name = name,
            .args = args,
            .signature = signature,
            .parent = parent,
            .kind = kind,
            .scope = scope,
            .flags = flags,
            .file = file,
            .token_class = token_class,
        };
    }

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

using SymbolTable = llvm::DenseMap<SymbolHash, Symbol>;

/// A symbol as queries hand it out: its identity plus the stored facts,
/// owned — the strings outlive whichever table answered.
struct SymbolInfo {
    SymbolHash hash = 0;
    std::string name;
    std::string args;
    std::string signature;
    SymbolHash parent = 0;
    SymbolKind kind;
    SymbolScope scope = SymbolScope::External;
    SymbolFlags flags = SymbolFlags::None;
    TokenClass token_class;

    static SymbolInfo from(SymbolHash hash, const SymbolView& identity) {
        return {
            .hash = hash,
            .name = identity.name.str(),
            .args = identity.args.str(),
            .signature = identity.signature.str(),
            .parent = identity.parent,
            .kind = identity.kind,
            .scope = identity.scope,
            .flags = identity.flags,
            .token_class = identity.token_class,
        };
    }

    /// The name with a specialization's arguments, as display shows it.
    std::string display_name() const {
        return name + args;
    }
};

/// A resolved document link: the argument range of an include-like
/// directive (byte offsets in the containing file) and the absolute path
/// of the target file. Plain data — it serializes over the worker RPC and
/// the PCH's pch.idx index as-is and becomes an LSP DocumentLink only
/// at the reply edge, where the session's line map does the conversion.
struct DocumentLink {
    LocalSourceRange range;
    std::string target;
};

}  // namespace clice::index
