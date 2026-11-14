/// What the index says about the scoped files, independent of any partition
/// into modules: the files and their includes, the entities they provide,
/// what each names of the others'.

module clice:analysis.facts;

import :index.symbol;
import :project.project;
import :semantic.symbol;
import :support.prelude;

namespace clice::analysis {

/// No file, entity, unit or include tree node.
constexpr inline std::uint32_t none = ~0u;

/// A file of the analyzed scope; its position in Facts::files is its id.
struct File {
    /// Relative to the project root, `/`-separated; absolute for a file
    /// outside the project, as a third-party header a scope glob takes in.
    std::string path;

    /// A translation unit's source file; every other scoped file is a header.
    bool source = false;

    /// A textual fragment (`.inc`, `.def`, ...) pasted into its includer:
    /// never a unit of its own, so what it names is charged to the files
    /// including it.
    bool fragment = false;

    std::uint32_t lines = 0;

    /// Distinct variants the index holds: more than one means the
    /// file reads differently depending on what includes it.
    std::uint32_t variants = 0;

    /// The scoped translation units that enter the file, itself included
    /// for a source.
    std::vector<std::uint32_t> units;

    /// The scoped files its include directives name, entered or skipped
    /// by a guard alike, and the scoped files naming it so.
    std::vector<std::uint32_t> includes;
    std::vector<std::uint32_t> includers;

    /// How each includer's directive names it, `<...>` or `"..."`, in
    /// `includers` order.
    std::vector<std::string> spellings;

    /// Its include directives the index saw enter or skip a scoped file:
    /// the directive's line and that file.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> directives;

    /// For a fragment, the includers whose Pasted relations at its directive
    /// name what it names there: it is a table pasted inside declarations,
    /// and its own entries are charged to no file.
    std::vector<std::uint32_t> pasters;

    /// Names only some of its variants carry: uses that follow the
    /// including context, such as the overloads a template's dependent
    /// call finds where it is included. They still draw edges.
    std::vector<std::string> unstable;

    /// Its variants declare different entities: a real dependence on the
    /// including context rather than lookup noise.
    bool declarations_differ = false;

    /// Out-of-scope templates a header specializes for arguments it does
    /// not provide (`std::formatter<llvm::StringRef>`): what instantiates
    /// them uses it without naming it, so no edge records those users.
    std::vector<std::string> specializes;
};

/// Why an entity has internal linkage.
enum class InternalLinkage : std::uint8_t {
    None,
    /// Declared `static`: TU-local once its header is a module interface.
    Static,
    /// Declared in an anonymous namespace: TU-local likewise.
    AnonymousNamespace,
    /// A namespace-scope const variable, internal only outside a module
    /// purview: no obstacle to the rewrite.
    Const,
};

/// How often a file names one entity, and the first line it does; 0 for
/// uses a hypothetical move supposes, which have no line.
struct Use {
    std::uint32_t entity = 0;
    std::uint32_t count = 0;
    std::uint32_t line = 0;
};

/// What a definition names, in the file it is written in.
struct BodyUse {
    std::uint32_t file = 0;
    std::uint32_t entity = 0;
    std::uint32_t count = 0;
};

/// An entity a scoped file provides to the files naming it.
struct Entity {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;

    /// The file providing it: a header defining it, else a header
    /// declaring it, the one most of the files naming it include, else the
    /// source defining it; the first by path among equals.
    std::uint32_t owner = 0;

    /// The file declaring it, whose index holds it: the owner, or the
    /// fragment the owner pastes it in from. A hypothetical move changes
    /// the owner, never this.
    std::uint32_t declared_in = 0;

    InternalLinkage linkage = InternalLinkage::None;

    /// The outermost enclosing entity the owner also provides (a member's
    /// class); the entity itself at namespace scope.
    std::uint32_t top = 0;

    /// The line of its first declaration in the owner, or in the fragment
    /// the owner pastes it in from.
    std::uint32_t line = 0;

    /// Times the owner names it beyond declaring it: a TU-local entity an
    /// inline function of its header uses is exposed by that function.
    std::uint32_t self_uses = 0;

    /// The first line the owner names it on beyond declaring it; 0 when
    /// only a fragment the owner pastes in does, or the owner is supposed.
    std::uint32_t self_line = 0;

    /// What its definitions in the owner and in the fragments the owner
    /// pastes in name, nested definitions apart, itself and the owner's
    /// other entities included: the uses that move with it.
    std::vector<BodyUse> body;

    /// A variable declared const or constexpr: a value, no state of its
    /// own per unit.
    bool constant = false;

    /// The units seeing its declaration, when the file declaring it reads
    /// differently across units (a header pasted once per implementation,
    /// a C header declaring into a namespace under C++); empty when that
    /// file has one reading.
    std::vector<std::uint32_t> units;

    /// Only some readings of the file declaring it declare it: the C
    /// library's <time.h> under __need_time_t lacks localtime_r.
    bool partial = false;

    /// The name a module interface exports it by: its own at namespace
    /// scope, `ns::E` for an enumerator of an unscoped enum at namespace
    /// scope; empty for a member, a specialization or a macro.
    std::string export_name;

    /// A macro's directive, continuation lines joined: what a macro header
    /// replays for the files importing its module.
    std::string directive;

    /// A macro definition some #undef removes: a helper scoped to the lines
    /// between, which no macro header carries.
    bool undefined = false;
};

/// A declaration or definition of an entity in a file other than its owner.
struct Redeclaration {
    std::uint32_t entity = 0;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    bool definition = false;

    /// A friend declaration rather than a forward declaration.
    bool friend_declaration = false;
};

/// A scoped file declaring an entity whose canonical declaration sits
/// outside the scope (`namespace llvm { class raw_ostream; }`): in a module
/// unit it would declare a second entity attached to that module.
struct ForeignDeclaration {
    std::string name;
    std::string owner;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    bool friend_declaration = false;
};

/// A macro a header names without including the file that defines it:
/// compiled alone, as a module interface is, the header reads differently.
struct ContextMacro {
    std::string name;
    std::string definition;
    std::uint32_t file = 0;
    std::uint32_t line = 0;

    /// The use sits in a preprocessor condition, so the difference is
    /// silent instead of a compile error.
    bool in_condition = false;
};

/// An explicit or partial specialization of a scoped template, written in
/// another file than the template.
struct Specialization {
    std::uint32_t primary = 0;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
};

/// A macro a scoped header defines that out-of-scope headers read, as a
/// configuration switch defined before a third-party include: a module
/// unit keeps it ahead of that include in its global module fragment.
struct ConfiguringMacro {
    std::uint32_t entity = 0;
    std::vector<std::string> readers;
};

/// An entity several headers define, as identical class definitions in
/// separate units may: as module interfaces they would be distinct
/// entities attached to distinct modules.
struct DuplicateDefinition {
    std::uint32_t entity = 0;
    std::vector<std::uint32_t> files;
};

/// A using-declaration a scoped file makes at namespace scope: the name it
/// declares there, which a module interface exports like any other.
struct UsingDeclaration {
    std::string name;
    std::uint32_t file = 0;
};

/// A namespace alias a scoped file declares at namespace scope. No
/// using-declaration exports an alias, so an interface redeclares it.
struct NamespaceAlias {
    std::string name;

    /// The namespace it names, qualified from the global namespace.
    std::string target;
    std::uint32_t file = 0;
};

struct ScopedIncludeTree {
    /// The unit, `none` out of scope.
    std::uint32_t unit = 0;

    /// Per node: the scoped file, `none` out of scope; the including node,
    /// `none` for the unit's own directives; whether a guard skipped it.
    std::vector<std::uint32_t> files;
    std::vector<std::uint32_t> parents;
    std::vector<char> skipped;
};

/// What the persisted index says about the scoped files, independent of
/// any partition into modules.
struct Facts {
    std::vector<File> files;
    llvm::StringMap<std::uint32_t> file_ids;

    std::vector<Entity> entities;

    /// Per file, the entities it names that another file owns: spelled
    /// names, names a macro expansion produces there, the overloads its
    /// templates' instantiations call by a qualified name, and the template
    /// an explicit specialization specializes.
    std::vector<std::vector<Use>> uses;

    /// Per file, the macros it expands or tests that another file defines.
    std::vector<std::vector<Use>> macro_uses;

    std::vector<Redeclaration> redeclarations;
    std::vector<ForeignDeclaration> foreign_declarations;
    std::vector<ContextMacro> context_macros;
    std::vector<Specialization> specializations;
    std::vector<DuplicateDefinition> duplicate_definitions;
    std::vector<ConfiguringMacro> configuring_macros;
    std::vector<NamespaceAlias> aliases;
    std::vector<UsingDeclaration> usings;
    /// Each unit's include tree, a node per file entry or guarded directive:
    /// a header's include edges merged over units report chains no single
    /// unit runs (<stdio.h> includes <stdarg.h> for its va_list type alone).
    std::vector<ScopedIncludeTree> trees;

    /// The files a file stands for: itself, or for a fragment the files
    /// including it, through fragments including fragments; with `pasted`
    /// false, none for a fragment some file pastes inside a declaration.
    llvm::SmallVector<std::uint32_t> including_files(std::uint32_t file, bool pasted) const;

    /// The files a file's uses are charged to: itself, or for a fragment the
    /// files including it. A table pasted inside declarations is charged to
    /// none: each file pasting it names what it names there through its own
    /// Pasted relations, and its own shard merges what every pasting context made of
    /// it (clang's Options.inc expands to enumerators in clang and to option
    /// tables naming the program's types in the program).
    llvm::SmallVector<std::uint32_t> charged_files(std::uint32_t file) const;

    /// File -> the fragments it pastes in, whose uses are charged to it.
    std::vector<llvm::SmallVector<std::uint32_t, 1>> pasted_fragments() const;
};

/// Read the facts of every indexed file whose path, relative to the project
/// root or absolute outside the project, `in_scope` accepts out of the
/// loaded index.
Facts collect(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope);

struct LoadedFacts {
    std::string root;
    Facts facts;
};

/// Why the index of a project gives no facts.
struct LoadError {
    std::string message;

    /// The files changed since they were indexed, sorted.
    std::vector<std::string> dropped;
};

/// The facts of the files the `scope` globs select out of the index of
/// `project` (default: the project's own files outside dot directories),
/// and the project root; none from an index that lacks a unit or describes
/// a file as it no longer is.
std::expected<LoadedFacts, LoadError> load_facts(Project& project,
                                                 llvm::ArrayRef<std::string> scope);

/// Move the entities of `symbols`, with their members, to the header
/// `path`, existing or hypothetical: what the move of a declaration would
/// do to the graph, without making it. `spec` names the move in errors.
std::expected<void, std::string> move_entities(Facts& facts,
                                               llvm::StringRef spec,
                                               llvm::ArrayRef<index::SymbolHash> symbols,
                                               llvm::StringRef path);

}  // namespace clice::analysis
