#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "compile/directive.h"
#include "semantic/symbol.h"
#include "syntax/lexical_scan.h"
#include "syntax/token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clang {

class Decl;
class Stmt;
class Attr;
class ConceptReference;
class CXXCtorInitializer;
class CXXBaseSpecifier;
class NamedDecl;

}  // namespace clang

namespace clice {

class CompilationUnitRef;

namespace types {

class TemplateResolver;

}

/// The single currency for "what a token belongs to": the AST node kinds our
/// traversal records, plus preprocessor and lexical entities, flattened into
/// one tagged union (no nested discriminant like wrapping clang::DynTypedNode
/// would introduce). Preprocessor payloads are pointers into the unit's
/// Directive storage, which shares the unit's lifetime; lexical payloads
/// (module declarations, comments) point into the Semantics' own LexicalInfo
/// storage.
class SemanticNode {
public:
    enum class Kind : std::uint8_t {
        Decl,
        Stmt,
        TypeLoc,
        NestedNameSpecifierLoc,
        CtorInitializer,
        BaseSpecifier,
        Attr,
        ConceptReference,
        TemplateArgumentLoc,

        /// #define
        MacroDefine,
        /// A macro expansion site or a conditional reference (#ifdef etc.).
        MacroReference,
        /// #undef
        MacroUndef,
        /// #include
        Include,
        /// C++20 module import.
        Import,
        /// A C++20 module declaration (any of its three lexical forms);
        /// neither the AST nor the preprocessor callbacks record it.
        Module,
        /// A comment; the spelled token stream drops them.
        Comment,
    };

    SemanticNode() = default;

    explicit SemanticNode(const clang::Decl* decl) : storage(decl) {}

    explicit SemanticNode(const clang::Stmt* stmt) : storage(stmt) {}

    explicit SemanticNode(clang::TypeLoc loc) : storage(loc) {}

    explicit SemanticNode(clang::NestedNameSpecifierLoc loc) : storage(loc) {}

    explicit SemanticNode(const clang::CXXCtorInitializer* init) : storage(init) {}

    explicit SemanticNode(const clang::CXXBaseSpecifier* base) : storage(base) {}

    explicit SemanticNode(const clang::Attr* attr) : storage(attr) {}

    explicit SemanticNode(const clang::ConceptReference* reference) : storage(reference) {}

    explicit SemanticNode(clang::TemplateArgumentLoc loc) : storage(loc) {}

    explicit SemanticNode(const MacroRef* macro) : storage(macro) {}

    explicit SemanticNode(const Include* include) : storage(include) {}

    explicit SemanticNode(const Import* import) : storage(import) {}

    explicit SemanticNode(const LexicalInfo::ModuleDeclaration* module) : storage(module) {}

    explicit SemanticNode(const LexicalInfo::Comment* comment) : storage(comment) {}

    Kind kind() const;

    /// Whether this node comes from the AST (as opposed to the preprocessor).
    bool is_ast() const {
        return kind() < Kind::MacroDefine;
    }

    /// Unified access. For Decl/Stmt/Attr subclasses this performs a dyn_cast,
    /// so e.g. `get<clang::VarDecl>()` works; other kinds match exactly.
    /// Returns nullptr when the kind does not match. Pointers to by-value
    /// alternatives (TypeLoc etc.) point into this node's storage.
    template <typename T>
    const T* get() const {
        if constexpr(std::derived_from<T, clang::Decl>) {
            if(auto decl = std::get_if<const clang::Decl*>(&storage)) {
                return llvm::dyn_cast<T>(*decl);
            }
        } else if constexpr(std::derived_from<T, clang::Stmt>) {
            if(auto stmt = std::get_if<const clang::Stmt*>(&storage)) {
                return llvm::dyn_cast<T>(*stmt);
            }
        } else if constexpr(std::same_as<T, clang::TypeLoc>) {
            return std::get_if<clang::TypeLoc>(&storage);
        } else if constexpr(std::same_as<T, clang::NestedNameSpecifierLoc>) {
            return std::get_if<clang::NestedNameSpecifierLoc>(&storage);
        } else if constexpr(std::same_as<T, clang::TemplateArgumentLoc>) {
            return std::get_if<clang::TemplateArgumentLoc>(&storage);
        } else if constexpr(std::same_as<T, clang::CXXCtorInitializer>) {
            if(auto init = std::get_if<const clang::CXXCtorInitializer*>(&storage)) {
                return *init;
            }
        } else if constexpr(std::same_as<T, clang::CXXBaseSpecifier>) {
            if(auto base = std::get_if<const clang::CXXBaseSpecifier*>(&storage)) {
                return *base;
            }
        } else if constexpr(std::derived_from<T, clang::Attr>) {
            if(auto attr = std::get_if<const clang::Attr*>(&storage)) {
                return llvm::dyn_cast<T>(*attr);
            }
        } else if constexpr(std::same_as<T, clang::ConceptReference>) {
            if(auto reference = std::get_if<const clang::ConceptReference*>(&storage)) {
                return *reference;
            }
        } else if constexpr(std::same_as<T, MacroRef>) {
            if(auto macro = std::get_if<const MacroRef*>(&storage)) {
                return *macro;
            }
        } else if constexpr(std::same_as<T, Include>) {
            if(auto include = std::get_if<const Include*>(&storage)) {
                return *include;
            }
        } else if constexpr(std::same_as<T, Import>) {
            if(auto import = std::get_if<const Import*>(&storage)) {
                return *import;
            }
        } else if constexpr(std::same_as<T, LexicalInfo::ModuleDeclaration>) {
            if(auto module = std::get_if<const LexicalInfo::ModuleDeclaration*>(&storage)) {
                return *module;
            }
        } else if constexpr(std::same_as<T, LexicalInfo::Comment>) {
            if(auto comment = std::get_if<const LexicalInfo::Comment*>(&storage)) {
                return *comment;
            }
        } else {
            static_assert(false, "unsupported type for SemanticNode::get");
        }

        return nullptr;
    }

    /// The source range identifying this node: the full range for AST nodes,
    /// the name token for macros, the directive location for includes, the
    /// keyword-to-name range for imports.
    clang::SourceRange source_range() const;

    /// Bridge for consumers still speaking clang::DynTypedNode (the
    /// selection tree's node payload). Only valid for AST kinds.
    clang::DynTypedNode dyn_typed() const;

    /// Visit the active alternative with a callable set.
    template <typename Visitor>
    decltype(auto) visit(Visitor&& visitor) const {
        return std::visit(std::forward<Visitor>(visitor), storage);
    }

private:
    std::variant<const clang::Decl*,
                 const clang::Stmt*,
                 clang::TypeLoc,
                 clang::NestedNameSpecifierLoc,
                 const clang::CXXCtorInitializer*,
                 const clang::CXXBaseSpecifier*,
                 const clang::Attr*,
                 const clang::ConceptReference*,
                 clang::TemplateArgumentLoc,
                 const MacroRef*,
                 const Include*,
                 const Import*,
                 const LexicalInfo::ModuleDeclaration*,
                 const LexicalInfo::Comment*>
        storage;
};

/// Tokens the selection algorithm never counts: comments, semicolons and
/// cvr-qualifiers (the AST does not model their locations).
bool should_ignore_token(const clang::syntax::Token& token);

/// A name occurrence a node gives rise to: the decl the written name refers
/// to, its role, and the name token's location.
struct NameOccurrence {
    const clang::NamedDecl* decl;

    RelationKind kind;

    clang::SourceLocation location;
};

/// The name occurrences of `node` — the single implementation of "node →
/// referenced decl" (the distilled content of the former SemanticVisitor
/// visit methods), shared by semantic tokens, the index projection and
/// hover.
///
/// Dependent names (typename T::type, unresolved lookups, dependent using
/// declarations) resolve through the template resolver into WeakReference
/// occurrences; without a resolver they produce nothing. Instantiation
/// decl heads produce no declaration occurrence (their locations repeat
/// the pattern), but nodes inside instantiated bodies deliberately do:
/// each instantiation acts as an implementation of the duck-typed
/// template, so a dependent name classifies as its actual resolutions —
/// see the "Instantiations as Implementations" section of the template
/// resolver design doc.
llvm::SmallVector<NameOccurrence, 2>
    resolve_occurrences(const SemanticNode& node, types::TemplateResolver* resolver = nullptr);

/// The semantic map of the interested file, built once after a successful
/// parse and serving every consumer that used to run its own traversal:
/// selection, semantic tokens, hover and the TUIndex projection.
///
/// It stores only structure — the recorded nodes and which spelled token
/// belongs to which node. Everything semantic (referenced decls, relations,
/// symbol identity) is derived on demand by the resolve_* function family:
/// the AST itself remains the single source of truth.
///
/// The map deliberately covers only what the live AST covers. Content the
/// compile consumed as immutable state (the preamble under a PCH) is served
/// by results precomputed at PCH build time and merged per feature. Do not
/// try to reconstruct it here: serialized rows cannot become AST nodes.
class Semantics {
public:
    constexpr static std::uint32_t invalid = static_cast<std::uint32_t>(-1);

    Semantics() = default;

    /// Move-only: nodes store payload pointers into the owned LexicalInfo,
    /// so a copy would silently keep pointing into the original.
    Semantics(const Semantics&) = delete;
    Semantics& operator=(const Semantics&) = delete;
    Semantics(Semantics&&) = default;
    Semantics& operator=(Semantics&&) = default;

    struct NodeFlags {
        /// The node is implicit (e.g. an implicit cast wrapper kept for
        /// parent chains).
        bool implicit : 1 = false;

        /// The node belongs to a template instantiation rather than the
        /// written template, so its locations point into the pattern.
        /// Instantiations reach the table as top-level implicit
        /// instantiations and as member subtrees of explicit instantiation
        /// directives; the directive's own decl and its written
        /// template-argument TypeLocs stay unflagged.
        bool in_instantiation : 1 = false;
    };

    struct Node {
        SemanticNode node;

        /// Index of the parent node. `invalid` for top-level AST nodes and,
        /// for now, for all preprocessor nodes.
        std::uint32_t parent = invalid;

        /// One past the last index of this node's subtree in the pre-order
        /// AST segment; node j is a descendant of i iff i < j < subtree_end.
        std::uint32_t subtree_end = invalid;

        /// Number of spelled tokens this node owns. A selection covering
        /// fewer than `owned` of them selects the node only partially.
        std::uint32_t owned = 0;

        NodeFlags flags;
    };

    /// Build the semantics of the unit: one full traversal claiming tokens
    /// innermost-first, plus one pass over the preprocessor directives.
    ///
    /// With interested_only (the shape features consume, cached on the unit)
    /// only the interested file's top-level decls are traversed. Without it
    /// the whole TU is traversed — the transient shape the full index
    /// projection uses; token ownership still only covers the interested
    /// file's spelled tokens.
    static Semantics build(CompilationUnitRef unit, bool interested_only = true);

    /// All recorded nodes in one index space: first the AST segment in DFS
    /// pre-order (a parent always precedes its children), then preprocessor
    /// nodes in source order.
    llvm::ArrayRef<Node> node_entries() const {
        return nodes;
    }

    const Node& node(std::uint32_t index) const {
        return nodes[index];
    }

    /// The spelled tokens of the interested file (a view into the unit's
    /// TokenBuffer, not a copy). They cover the whole file even under a
    /// preamble PCH — what the PCH consumes is the preamble's AST and
    /// directives (those travel through PreambleState instead), not its
    /// spelling.
    llvm::ArrayRef<clang::syntax::Token> spelled_tokens() const {
        return tokens;
    }

    /// Byte offset of spelled token `index` in the interested file.
    std::uint32_t token_offset(std::uint32_t index) const {
        return tokens[index].location().getRawEncoding() - file_begin.getRawEncoding();
    }

    /// Whether spelled token `index` was preprocessed away to nothing
    /// (a disabled region or an empty expansion).
    bool token_preprocessed_away(std::uint32_t index) const {
        return pp_ignored[index];
    }

    /// The nodes owning spelled token `index`. Almost always a single node;
    /// macro names and include filenames may map to several.
    llvm::ArrayRef<std::uint32_t> owners(std::uint32_t index) const {
        return llvm::ArrayRef(owner_nodes)
            .slice(owner_begin[index], owner_begin[index + 1] - owner_begin[index]);
    }

    /// The interested file's comments in source order (the spelled token
    /// stream drops them). The same objects back the Comment nodes.
    llvm::ArrayRef<LexicalInfo::Comment> comments() const {
        return lexical.comments;
    }

    /// The interested file's module declarations, already cross-checked
    /// against the compiled module (bogus lexical matches are dropped at
    /// build time). The same objects back the Module nodes.
    llvm::ArrayRef<LexicalInfo::ModuleDeclaration> module_declarations() const {
        return lexical.modules;
    }

private:
    friend class SemanticsBuilder;

    std::vector<Node> nodes;

    llvm::ArrayRef<clang::syntax::Token> tokens;

    /// Start location of the interested file, for O(1) offset computation.
    clang::SourceLocation file_begin;

    /// Tokens preprocessed to nothing.
    std::vector<bool> pp_ignored;

    /// CSR layout: the owners of token i are
    /// owner_nodes[owner_begin[i] .. owner_begin[i + 1]).
    std::vector<std::uint32_t> owner_begin;
    std::vector<std::uint32_t> owner_nodes;

    /// The lexically scanned entities; Module and Comment nodes point into
    /// this storage.
    LexicalInfo lexical;
};

/// resolve_occurrences with tree context: a dependent name that is the
/// callee of a call (found through the node's parent chain) has its
/// candidate set filtered by the call's arity.
llvm::SmallVector<NameOccurrence, 2>
    resolve_occurrences(const Semantics& semantics,
                        std::uint32_t index,
                        types::TemplateResolver* resolver = nullptr);

}  // namespace clice
