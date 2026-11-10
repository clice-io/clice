/// The reading of a project's index into facts: the passes collect runs, in
/// order, and the state each leaves for the next.

module clice:analysis.facts.builder;

import :analysis.facts;
import :analysis.text;
import :index.project_index;
import :index.shard;
import :index.symbol;
import :index.tu_manifest;
import :project.project;
import :support.prelude;
import :syntax.token;
import :vfs.file_table;
import :vfs.ids;

namespace clice::analysis {

/// A scoped file's declaration of an entity.
struct Site {
    std::uint32_t file = 0;
    std::uint32_t line = 0;

    /// Where the declared name starts.
    std::uint32_t offset = 0;
    bool definition = false;

    /// The declaration's extent, empty when the index holds none.
    LocalSourceRange extent{0, 0};
};

/// How often a file names a symbol, and the first line it does.
struct RawUse {
    std::uint32_t count = 0;
    std::uint32_t line = 0;

    /// Some relation names it outright rather than as an overload candidate.
    bool strong = false;
};

/// Visit each identifier of `code`.
void for_each_identifier(const std::string& code, llvm::function_ref<void(llvm::StringRef)> visit);

struct FactsBuilder {
    FactsBuilder(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope);

    /// The scoped files: the indexed ones, those holding a shard, the
    /// translation units and the files their include trees enter, by path.
    void list_files();

    /// The units' include trees, and every file's include edges.
    void read_includes();

    /// What each scoped file's shard declares and names.
    void read_shards();

    /// What each scoped file's template instantiations call by a
    /// namespace-qualified name.
    void read_instantiations();

    /// The includers whose Pasted relations name what a fragment names.
    void find_pasters();

    /// The macro definitions some #undef ends, and where each macro name is
    /// defined.
    void read_macros();

    /// The using-declarations a scoped file makes at namespace scope.
    void find_usings();

    /// The namespace aliases a scoped file declares at namespace scope.
    void find_aliases();

    /// Every entity the scoped files declare or name, each attributed to
    /// the file providing it.
    void read_entities();

    /// What explicit and partial specializations specialize.
    void read_specializations();

    /// What each file names of the others' entities, and what each
    /// definition names.
    void read_uses();

    /// The macros a header names without including their definition.
    void find_context_macros();

    /// The path of a file relative to the project root, absolute outside
    /// it.
    std::string display(Fid fid) const;

    /// The scoped file `fid` is, none for one out of scope.
    std::uint32_t id_of(Fid fid) const;

    /// A scoped file's text, from the disk (the index holds positions, not
    /// the source), by its 1-based lines.
    const Text& text_of(std::uint32_t file);

    /// The code tokens of 1-based `line` of `file`.
    llvm::ArrayRef<Token> tokens_on(std::uint32_t file, std::uint32_t line);

    /// Whether `line` of `file` writes the identifier `word` in its code.
    bool writes(std::uint32_t file, std::uint32_t line, llvm::StringRef word);

    /// A directive's text, its continuation lines joined.
    std::string directive_text(std::uint32_t file, std::uint32_t line);

    /// The symbol, from the index or, for a TU-local one, from `shard`, the
    /// shard holding the entity's entries.
    std::optional<index::SymbolView> find_symbol(index::SymbolHash hash,
                                                 const index::Shard* shard) const;

    /// A name qualified by its containers, inline namespaces skipped as
    /// lookup skips them.
    std::string qualified_name(index::SymbolHash hash, const index::Shard* shard) const;

    /// The name a module interface exports an entity by: its own at
    /// namespace scope; an unscoped enum's enumerator there by the enclosing
    /// namespace, which the enum declares it in.
    std::string export_name_of(index::SymbolHash hash,
                               const index::SymbolView& identity,
                               const index::Shard* shard) const;

    /// Whether `site` declares a friend, its keyword on a line of the
    /// declaration up to the name: from the extent's first line, or with
    /// none the line before when the name opens its own; or in a macro
    /// those lines expand, as toml++ spells its comparison friends.
    bool friend_site(const Site& site, llvm::StringRef name);

    /// The namespace a position of a file sits in, from the extents of the
    /// declarations there, `self`'s own apart: "" for the global one, none
    /// inside a class or a function.
    std::optional<std::string> namespace_at(std::uint32_t file,
                                            std::uint32_t offset,
                                            index::SymbolHash self) const;

    /// Classify `hash` once: the entity it is, with the file providing it,
    /// or none for what links no files, is declared outside the scope or
    /// only named.
    void classify(index::SymbolHash hash);

    /// The file providing the entity `hash`, declared at `scoped` and
    /// canonically in `canonical`; none for one declared outside the scope,
    /// whose scoped declarations are recorded as foreign. A fragment's
    /// entity goes to the file pasting it in.
    std::uint32_t owner_of(index::SymbolHash hash,
                           const index::SymbolView& identity,
                           llvm::ArrayRef<Site> scoped,
                           Fid canonical);

    /// Of the files including `fragment`, the one providing the entity
    /// `hash` the fragment declares at its site among `scoped`; none when no
    /// file includes it.
    std::uint32_t paster_of(std::uint32_t fragment,
                            index::SymbolHash hash,
                            const index::SymbolView& identity,
                            llvm::ArrayRef<Site> scoped);

    /// A member's top: the outermost container its owner also provides.
    void link_members();

    /// The entity `hash` is, none for one classify dropped.
    std::uint32_t entity_of(index::SymbolHash hash) const;

    /// The include closure of a file, over every include edge, guard-skipped
    /// ones too: a skipped directive still names the file.
    llvm::DenseSet<Fid> include_closure(std::uint32_t file) const;

    index::ProjectIndex& index;
    FileTable& table;
    llvm::StringRef root;
    llvm::function_ref<bool(llvm::StringRef)> in_scope;

    Facts facts;

    /// Per Fid met, its scoped file, none out of scope; per scoped file,
    /// its Fid.
    llvm::DenseMap<Fid, std::uint32_t> ids;
    std::vector<Fid> fids;

    /// The units' manifests, by the units' paths.
    std::vector<std::pair<Fid, index::TUManifest>> manifests;

    /// Include edges of every file, scoped or not: a header's include
    /// closure runs through third-party headers.
    llvm::DenseMap<Fid, llvm::SmallVector<Fid>> includes;
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 2>> included_by;

    /// (file, variant hash) -> the scoped units whose index of the file is
    /// that variant.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint64_t>, llvm::SmallVector<std::uint32_t, 2>>
        contributors;

    /// (includer, directive line) -> the file the directive includes.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, Fid> include_at;

    struct Source {
        std::string content;
        std::optional<Text> text;
    };

    llvm::DenseMap<std::uint32_t, std::unique_ptr<Source>> sources;

    /// Per symbol, its declarations in scoped files.
    llvm::DenseMap<index::SymbolHash, llvm::SmallVector<Site, 2>> sites;

    /// What each scoped file names, from its reference relations: a spelled
    /// name mirrors its occurrence there, and a name a macro expansion
    /// produces has only the relation. A weak one, a candidate or a guess,
    /// may be one overload a dependent call does not run, kept apart: the
    /// candidates an operator finds where a template is defined are not
    /// what its instantiations call. What an instantiation calls by a
    /// qualified name is named as a spelled name is.
    std::vector<llvm::DenseMap<index::SymbolHash, RawUse>> raw;

    /// Per file, the lines of its Pasted relations.
    std::vector<llvm::DenseSet<std::uint32_t>> pasted_lines;

    /// Per file, its use relations, and its weak ones apart, by offset.
    std::vector<std::vector<std::pair<std::uint32_t, index::SymbolHash>>> use_sites;
    std::vector<std::vector<std::pair<std::uint32_t, index::SymbolHash>>> weak_sites;

    /// (file, entity) -> the units whose variant of the file declares it,
    /// for a file read differently across units.
    llvm::DenseMap<std::pair<std::uint32_t, index::SymbolHash>, llvm::SmallVector<std::uint32_t>>
        declaring_units;

    /// (file, entity) declared by only some of the file's variants.
    llvm::DenseSet<std::pair<std::uint32_t, index::SymbolHash>> partially_declared;

    /// Specialization, primary template, file.
    std::vector<std::tuple<index::SymbolHash, index::SymbolHash, std::uint32_t>>
        raw_specializations;

    /// The macro definitions some #undef removes.
    llvm::DenseSet<index::SymbolHash> undefined;

    /// Macro name -> the (file, line) of each scoped definition.
    llvm::StringMap<llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>, 1>>
        macro_definitions;

    /// Per file, the extents of the declarations there.
    std::vector<std::vector<std::pair<LocalSourceRange, index::SymbolHash>>> definitions;

    /// Every hash some scoped file names or declares, classified once:
    /// its entity, none for what classify drops.
    llvm::DenseMap<index::SymbolHash, std::uint32_t> entity_ids;

    /// Macros defined outside the scope that scoped files use: the file
    /// defining each and its name.
    llvm::DenseMap<index::SymbolHash, std::pair<Fid, std::string>> foreign_macros;

    /// Per entity, its parent symbol and whether it is an operator.
    std::vector<index::SymbolHash> parents;
    std::vector<bool> operators;
};

}  // namespace clice::analysis
