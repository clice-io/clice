module clice:index.indexing.builder;

import :compile.compilation;
import :index.indexing;
import :index.indexing.include_tree;
import :index.indexing.layout;
import :index.shard.instantiations;
import :index.symbol;
import :semantic.display;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.template_resolver;
import :support.prelude;

namespace clice::index {

/// Projects the unit's semantic map into TUIndex entries: occurrences and
/// relations from the resolve facts, macros from the preprocessor directives.
class Builder {
public:
    Builder(CompilationUnit& unit, const TUIndexOptions& options);

    /// The TU index, into `blob`, which arrives holding what a preamble's
    /// own index adds on top of an ordinary build when `preamble` is set.
    std::vector<std::byte> build();

private:
    /// What this run's instantiations and macro expansions resolve in each
    /// file, and the known keys whose definition the file holds.
    struct Resolved {
        FileInstantiations walked;
        std::vector<SymbolHash> skipped;
    };

    /// The names the written code spells through a macro: the ones a
    /// `#define` body spells make the expansions' resolutions.
    struct BodyName {
        const clang::NamedDecl* decl;
        RelationKind kind;
        clang::SourceLocation location;
    };

    /// The written relations of one file, by what a record would say: an
    /// instantiation resolves only what its template's own entries leave
    /// open there. Empty for a file this run does not index — a Content
    /// compile's headers, whose records then stand alone.
    using WrittenKey =
        std::tuple<SymbolHash, std::uint32_t, std::uint32_t, std::uint32_t, SymbolHash>;

    /// The only gate through which entries enter `file_indices`. With
    /// focus_only, the index covers just the focus file and the
    /// declarations project_imported adds last — yet AST nodes reachable
    /// from its top-level decls can carry locations in other files:
    /// inherited default arguments and base specifiers of classes defined
    /// in a preamble header end up there. Such entries belong to the
    /// preamble's own index, so they are dropped here. So are the entries
    /// of a header's borrowed includer context: its host indexes them.
    FileIndex* file_index(clang::FileID fid);

    /// Symbol-table registration shared by every occurrence and relation
    /// writer: name lookups need the entry even when a symbol has only
    /// relations.
    index::SymbolHash ensure_symbol(const clang::NamedDecl* decl);

    /// What the symbol table records about a declaration besides its
    /// index entries. The facts the entries decide — HasDefinition and the
    /// canonical file — are filled in once every entry is projected.
    Symbol describe(const clang::NamedDecl* decl);

    void add_occurrence(const clang::NamedDecl* decl, clang::SourceRange name);

    /// The symbol of the macro `hash` names, its symbol-table entry made on
    /// first sight: like a declaration's, without it build() would
    /// default-construct a nameless entry when recording reference files,
    /// and every name lookup for the macro would come back empty. `def` is
    /// null for a name no definition resolves.
    SymbolHash ensure_macro(SymbolHash hash,
                            clang::SourceLocation name,
                            const clang::MacroInfo* def);

    void add_macro(const clang::MacroInfo* def, RelationKind kind, clang::SourceLocation location);

    /// A macro name a directive spells where no definition resolves it: a
    /// reference of the name's own symbol, beside no occurrence — the
    /// cursor there stands on no macro.
    void add_unresolved_macro(clang::SourceLocation location);

    /// Record a relation of `decl` in the index of `fid`, `target` naming
    /// the symbol it leads to. A file file_index drops takes the relation
    /// with it, and neither symbol is registered.
    void relate(clang::FileID fid,
                const clang::NamedDecl* decl,
                Relation relation,
                const clang::NamedDecl* target = nullptr);

    /// A Definition/Declaration/Reference relation mirroring an occurrence:
    /// it spans the whole written name the occurrence lies in, and decl/def
    /// relations also carry the declaration's full extent.
    void add_self_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           clang::SourceRange name);

    /// The Instantiation relation of an explicit instantiation, at its name
    /// (a macro's invocation that writes none), with the directive's extent.
    void add_instantiation(const clang::ExplicitInstantiationDecl& directive);

    /// A symbol-to-symbol relation (type-of, inheritance, overrides, ctor/dtor
    /// ownership); `anchor` decides which file's index receives it.
    void add_pair_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           const clang::NamedDecl* target,
                           clang::SourceRange anchor);

    /// The TypeDefinition relations of a value (a function's: its return
    /// value): the type it is declared with, and for a smart pointer also
    /// the type it points to.
    void add_type_definitions(const clang::ValueDecl* decl);

    /// A member of a class template's specialization, or a variable
    /// template's specialization, can have another type than its pattern
    /// declares (`T value` is a Widget in `Box<Widget>`): at a name of one,
    /// a TypeDefinition relation spanning the name holds that type, beside
    /// the pattern's own, which carries no range.
    void add_specialized_type(const clang::NamedDecl* decl, clang::SourceRange name);

    /// A call edge, recorded at the call expression's location.
    void add_call_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           const clang::NamedDecl* target,
                           clang::SourceRange range);

    /// Module names are indexed like macro names: an occurrence plus a
    /// Definition/Declaration/Reference relation keyed by the module's
    /// entity, so navigation flows through the ordinary index pipeline.
    void index_modules(const Semantics& semantics);

    /// The nearest enclosing function of node `index`, for call edges. Methods
    /// count as callers too (the previous traversal-stack implementation
    /// missed them: methods take a different RAV path than TraverseFunctionDecl).
    ///
    /// Memoized with path compression: calls nested under a deep expression
    /// chain would otherwise each rescan thousands of the same ancestors.
    const clang::NamedDecl* enclosing_function(const Semantics& semantics, std::uint32_t index);

    /// The namespace-scope declaration node `index` lies in, null outside
    /// any. Memoized like enclosing_function.
    const clang::Decl* enclosing_top_level(const Semantics& semantics, std::uint32_t index);

    /// The include inside `host` that pastes in the fragment holding
    /// `location` (`#include "body.inc"` inside a function); invalid when
    /// `location` lies in `host`'s file itself or nothing inside `host`
    /// includes it.
    clang::SourceLocation pasted_into(const clang::Decl* host, clang::SourceLocation location);

    /// Hierarchy ranges are positions in the caller's document: a call
    /// written in a fragment the caller's body pastes in is recorded at
    /// that include.
    clang::SourceRange in_caller_file(const clang::NamedDecl* caller, clang::SourceRange range);

    /// A fragment pasted inside a declaration (an X-macro table included
    /// into a switch) is the pasting file's text: each name it uses gets a
    /// Pasted relation at that include, while its own entries stay where
    /// the fragment spells it.
    void add_pasted_relation(const clang::NamedDecl* decl,
                             const clang::Decl* host,
                             clang::SourceLocation location);

    /// Decl-pair relation facts: type definitions, inheritance, overrides,
    /// constructor/destructor ownership and call edges. Only the index
    /// consumes these, so they live here rather than in the semantic layer.
    void project_relations(const Semantics& semantics, std::uint32_t index);

    /// A keyword standing for a type it does not spell — `auto`,
    /// `decltype(auto)`, `decltype(expr)`, an abbreviated template's
    /// `auto` — is an occurrence of the declarations that type names once
    /// its pointers, references and arrays are stripped and a library's
    /// internal aliases seen through (`std::__detail::__unique_ptr_t`), as if
    /// the type were written there, with a Deduced relation: navigation from
    /// the keyword reaches the type, references to the type never list the
    /// keyword.
    void project_placeholder(const Semantics& semantics, std::uint32_t index);

    /// The entries every name occurrence makes: the occurrence where its
    /// token spells the name, and the self-relation spanning the whole
    /// name, whose range it returns.
    clang::SourceRange add_name(const NameOccurrence& occurrence);

    /// A header the focus file names something from has its entries in the
    /// preamble's index; a module unit has them only once its own unit is
    /// indexed. Until then, its declarations of what the focus file names
    /// come from the AST the import deserialized, as that unit's index
    /// records them.
    void project_imported();

    /// Answer the resolver's queries from node `i` on as its place sees the
    /// unit. A header's entries are what the header sees on its own, as
    /// every unit including it shares them; what a declaration holds, met
    /// after it in the walk, sees what the declaration does.
    void see_from(const Semantics& semantics, std::uint32_t i);

    void project_node(const Semantics& semantics, std::uint32_t i);

    /// In an instantiation, a variable or a parameter its template declares
    /// with a placeholder type (`auto x`, an abbreviated template's `auto`
    /// parameter or pack): the types go-to-type-definition reaches from it,
    /// at its name — beside the template's own, which carries none.
    void add_deduced_type(const clang::ValueDecl* decl);

    /// The pattern's own declaration of a parameter or a local an
    /// instantiated body declares: instantiation copies its location.
    const clang::NamedDecl* in_pattern(const clang::NamedDecl* decl);

    /// The file holding the definition an instantiation was made from: what
    /// it resolves lies there. The pattern clang answers for a function or
    /// a variable can be a declaration the unit's lookup met (`std::min`
    /// redeclared in algorithmfwd.h after its definition), which varies
    /// with the unit; the definition does not.
    clang::FileID home_of(const clang::Decl* decl);

    /// Whether a file's instantiations belong to this run's index: not to
    /// a borrowed includer context, whose host indexes them.
    bool resolves_in(clang::FileID fid);

    /// The function and variable instantiations to walk; the known ones a
    /// file holds go to its skipped keys.
    std::vector<const clang::Decl*> walked_instantiations();

    /// The top-level declarations the focus file does not own that expand a
    /// macro it defines: `#define TYPE isl_space` before `#include
    /// "templ.c"`, or `static` before an `#include` writing the rest of the
    /// function. What the body names there is a resolution of the focus
    /// file, which a walk of its own declarations does not reach.
    std::vector<const clang::Decl*> expanding_decls();

    static WrittenKey written_key(SymbolHash symbol, const Relation& relation);

    const llvm::DenseSet<WrittenKey>& written_in(clang::FileID fid);

    /// The records one instantiation's entries in its home file amount to,
    /// beyond what the written entries there say.
    std::vector<InstantiationRecord> records_of(clang::FileID home, const FileIndex& entries);

    /// One walked instantiation: its key in its home file, and the resolution
    /// its records make there, if any.
    void add_walked(clang::FileID home,
                    const clang::NamedDecl* decl,
                    std::vector<InstantiationRecord> records);

    void project_instantiations(llvm::ArrayRef<const clang::Decl*> walked);

    /// What the walk of instantiated definition `decl` projected into
    /// `entries`, kept as its walked records.
    void close_instantiation(const clang::NamedDecl* decl,
                             llvm::DenseMap<clang::FileID, FileIndex>& entries);

    /// A class instantiation's bases, where its template's base specifiers
    /// depend on the arguments (`struct D : T`): no key, the resolution alone —
    /// computing them costs less than looking one up.
    void project_bases(const clang::CXXRecordDecl* record);

    /// What one expansion's `#define` body spells — the names of the
    /// declarations and macros it reaches, in the body's text — a resolution
    /// of the macro's file without a key: the same body resolves differently
    /// from one expansion to the next (`LLVM_DEBUG` and each file's
    /// `DEBUG_TYPE`).
    void project_expansions();

    /// What node `index` of a focus compile's other files' written
    /// declarations names through the focus file's `#define` bodies; their
    /// own entries are their files' to index.
    void collect_body_names(const Semantics& path, std::uint32_t index);

    void project_macros();

private:
    CompilationUnit& unit;

    bool focus_only;

    llvm::ArrayRef<std::uint64_t> known_variants;

    types::TemplateResolver resolver;

    /// The resolver for the place see_from set; none in a system header
    /// other than the focus file. Every unit including a library would
    /// resolve its templates' bodies again, the most of the resolver's work
    /// in a unit, for names only a reader of the library's own code asks
    /// about: opening the header makes it the focus file.
    types::TemplateResolver* place_resolver = &resolver;

    /// The types function signatures print as readers know them.
    display::ReadableTypes readable;

    IncludeTree tree;

    SymbolTable symbols;

    /// The macro definitions an `#undef` in their own file ends: including
    /// that file leaves none of them defined.
    llvm::DenseSet<const clang::MacroInfo*> undefined;

    /// Build-time working state keyed by FileID — clang::FileID means
    /// nothing outside the compilation, so it never leaves the builder;
    /// the encode step converts it through tree.path_id.
    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    /// With focus_only, the symbols registered that an imported
    /// module declares, a namespace aside: every unit reopening it
    /// redeclares it.
    llvm::SmallVector<const clang::NamedDecl*> imported;

    /// The declaration describing each symbol registered below external
    /// scope.
    llvm::DenseMap<SymbolHash, const clang::NamedDecl*> internal_decls;

    llvm::DenseSet<clang::FileID> module_files;

    /// enclosing_function's and enclosing_top_level's answers by node: a
    /// whole-TU walk numbers the nodes of each path anew, so project_node
    /// forgets them from its node's index on.
    std::vector<std::optional<const clang::NamedDecl*>> enclosing_memo;

    std::vector<std::optional<const clang::Decl*>> top_level_memo;

    /// The keys of the instantiations the receiver already holds
    /// (TUIndexOptions::known_instantiations).
    llvm::ArrayRef<std::uint64_t> known_instantiations;

    /// Whether the standard library's own instantiations and expansions are
    /// walked (TUIndexOptions::standard_library_instantiations).
    bool standard_library;

    /// See TUIndexOptions::library_bodies.
    bool library_bodies;

    llvm::DenseMap<clang::FileID, Resolved> resolved;

    /// Per file, its written relations as records would say them
    /// (written_in).
    llvm::DenseMap<clang::FileID, llvm::DenseSet<WrittenKey>> written;

    /// While an instantiation's nodes project, the entries they make:
    /// file_index hands these out instead of the files' own.
    llvm::DenseMap<clang::FileID, FileIndex>* instantiation_entries = nullptr;

    /// The instantiated definition, when it is a function: the caller of
    /// its calls.
    const clang::FunctionDecl* instantiated_function = nullptr;

    /// Where the expression being projected begins (in_pattern).
    clang::SourceLocation expression_begin;

    /// Per pattern function, its parameters and locals by location and kind.
    llvm::DenseMap<
        const clang::FunctionDecl*,
        llvm::DenseMap<std::pair<clang::SourceLocation::UIntTy, unsigned>, const clang::NamedDecl*>>
        pattern_locals;

    std::vector<BodyName> body_names;
};

}  // namespace clice::index
