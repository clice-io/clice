#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "analysis/annotation.h"
#include "index/types.h"
#include "semantic/symbol.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct Project;

}

namespace clice::analysis {

/// A file of the analyzed scope; its position in Facts::files is its id.
struct File {
    /// Workspace-relative, `/`-separated.
    std::string path;

    /// A translation unit's main file; every other scoped file is a header.
    bool source = false;

    /// A textual fragment (`.inc`, `.def`, ...) pasted into its includer:
    /// never a unit of its own, so what it names is charged to the files
    /// including it.
    bool fragment = false;

    std::uint32_t lines = 0;

    /// Distinct row variants the index holds: more than one means the
    /// file reads differently depending on what includes it.
    std::uint32_t variants = 0;

    /// The scoped translation units that enter the file, itself included
    /// for a source.
    std::vector<std::uint32_t> units;

    /// The scoped files its include directives name, entered or skipped
    /// by a guard alike, and the scoped files naming it so.
    std::vector<std::uint32_t> includes;
    std::vector<std::uint32_t> includers;

    /// Names only some of its row variants carry: uses that follow the
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

/// How often a file names one entity, and the first line it does.
struct Use {
    std::uint32_t entity = 0;
    std::uint32_t count = 0;
    std::uint32_t line = 0;
};

/// An entity a scoped file provides to the files naming it.
struct Entity {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;

    /// The file providing it: a header defining it, else a header
    /// declaring it, else the source defining it; the first by path among
    /// equals.
    std::uint32_t owner = 0;

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

    /// What its definition in the owner names, nested definitions apart,
    /// itself and the owner's other entities included: the uses that move
    /// with it.
    std::vector<Use> body;
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

/// What the persisted index says about the scoped files, independent of
/// any partition into modules.
struct Facts {
    std::vector<File> files;
    llvm::StringMap<std::uint32_t> file_ids;

    std::vector<Entity> entities;

    /// Per file, the entities it names that another file owns: spelled
    /// names, names a macro expansion produces there, and the template an
    /// explicit specialization specializes.
    std::vector<std::vector<Use>> uses;

    /// Per file, the macros it expands or tests that another file defines.
    std::vector<std::vector<Use>> macro_uses;

    std::vector<Redeclaration> redeclarations;
    std::vector<ForeignDeclaration> foreign_declarations;
    std::vector<ContextMacro> context_macros;
    std::vector<Specialization> specializations;
    std::vector<DuplicateDefinition> duplicate_definitions;
    std::vector<ConfiguringMacro> configuring_macros;
};

/// Read the facts of every indexed file whose workspace-relative path
/// `in_scope` accepts out of the loaded index.
Facts collect(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope);

/// An assignment of every scoped file to a module.
struct Partition {
    std::vector<std::string> modules;
    std::vector<std::uint32_t> module_of;

    /// The module so named; `modules.size()` for none.
    std::uint32_t module_named(llvm::StringRef name) const {
        return static_cast<std::uint32_t>(std::ranges::find(modules, name) - modules.begin());
    }
};

struct PartitionSpec {
    /// Directory segments that name a file's default module; 0 for its
    /// whole directory.
    std::uint32_t depth = 0;

    /// Modules claimed by globs over workspace-relative paths, first match
    /// wins; unclaimed files keep their directory module.
    std::vector<std::pair<std::string, std::vector<std::string>>> modules;

    /// `path=module` reassignments, applied after the globs.
    std::vector<std::string> moves;

    /// `a+b+c`: b's and c's files join a.
    std::vector<std::string> merges;
};

std::expected<Partition, std::string> partition(const Facts& facts, const PartitionSpec& spec);

/// Move the entities a `<name or #id>=<path>` spec selects, with their
/// members, to another header, existing or hypothetical: what the move
/// of a declaration would do to the graph, without making it.
std::expected<void, std::string> move_entities(Facts& facts, llvm::StringRef spec);

struct ModuleSummary {
    std::string name;
    std::uint32_t headers = 0;
    std::uint32_t sources = 0;
    std::uint32_t lines = 0;

    /// 0 for a module importing no other; else one above its highest
    /// import. Modules of one cycle share a layer.
    std::uint32_t layer = 0;
};

struct EdgeSummary {
    std::string from;
    std::string to;

    /// Distinct entities the from-module's headers name: the edge its
    /// interface would import.
    std::uint32_t interface_entities = 0;

    /// Distinct entities only its sources name: implementation units may
    /// import upward, so these never close a cycle.
    std::uint32_t implementation_entities = 0;

    std::vector<std::string> sample;
};

struct CutEdge {
    std::string from;
    std::string to;
    std::uint32_t entities = 0;
    std::vector<std::string> sample;
    std::vector<std::string> files;
};

/// A strongly connected set of modules over interface edges, with the
/// lightest set of edges whose removal breaks it, weighed by distinct
/// entities to move.
struct Cycle {
    std::vector<std::string> modules;
    std::vector<CutEdge> cut;
};

struct Impact {
    std::string path;

    /// Rebuild weight of an edit today: the scoped units entering the file.
    double baseline = 0;

    /// Under the partition: the units of every module whose interface
    /// imports the file's module, transitively, and of the sources
    /// importing any of them — a BMI records the hash of each BMI it
    /// imports, so an interface edit travels the whole importer closure.
    double partitioned = 0;

    /// The churn annotation, when present.
    std::optional<double> churn;

    /// An internal partition under the partition.
    bool internal = false;
};

struct Totals {
    /// Σ churn × rebuild weight over every scoped file; with no churn
    /// annotation every file counts once.
    double baseline = 0;
    double partitioned = 0;
};

/// A header that, with the sources implementing it and the other headers
/// those implement, exchanges nothing with the rest of its module and
/// whose users all sit in one other module: private to that module.
struct MoveCandidate {
    std::string path;
    std::string from;
    std::string to;

    /// What moves along: the sources implementing it and the other headers
    /// they implement.
    std::vector<std::string> with;

    /// Distinct entities it exchanges with the target, both directions.
    std::uint32_t entities = 0;

    /// Modules on cycles after the move.
    std::uint32_t cyclic_after = 0;
};

struct SplitPart {
    /// The first ten by line, as `name:line`, of `count`.
    std::vector<std::string> entities;
    std::uint32_t count = 0;
    std::vector<std::string> consumers;
};

/// A header whose namespace-scope entities fall into groups no consuming
/// module shares; a class stays whole with its members.
struct SplitCandidate {
    std::string path;
    std::string module;
    std::vector<SplitPart> parts;
};

struct ObstacleCounts {
    /// Those whose variants declare different entities.
    std::uint32_t variant_headers = 0;
    std::uint32_t context_macros = 0;
    std::uint32_t borrowed_macros = 0;
    std::uint32_t internal_uses = 0;
    std::uint32_t cross_module_redeclarations = 0;
    std::uint32_t foreign_declarations = 0;
    std::uint32_t split_definitions = 0;
    std::uint32_t duplicate_definitions = 0;
    std::uint32_t configuring_macros = 0;
    std::uint32_t implicit_providers = 0;
    std::uint32_t specializations = 0;
};

/// Headers of one module naming each other's entities in a cycle: as
/// partitions they cannot import each other, so they become one partition
/// or the cycle is broken. A use the header backs with its own forward
/// declaration does not count.
struct PartitionCycle {
    std::string module;
    std::vector<std::string> headers;
};

struct Overview {
    std::vector<std::string> annotations;
    std::uint32_t files = 0;
    std::vector<ModuleSummary> modules;
    std::vector<EdgeSummary> edges;
    std::vector<Cycle> cycles;

    /// Modules on cycles.
    std::uint32_t cyclic_modules = 0;
    std::vector<PartitionCycle> partition_cycles;

    /// Headers only their own module's sources use, directly or through
    /// other such headers: the generator makes them internal partitions,
    /// which import nothing into the module's interface and whose edits
    /// rebuild only their users.
    std::uint32_t internal_headers = 0;
    std::vector<std::string> deepest_chain;
    Totals totals;
    std::vector<Impact> hotspots;
    std::vector<MoveCandidate> moves;
    std::vector<SplitCandidate> splits;
    ObstacleCounts obstacles;
};

struct EntityUse {
    /// `#<hex>`, as `clice query` takes it; overloads share a name.
    std::string id;
    std::string entity;
    std::string owner;
    std::vector<std::string> users;
    bool interface = false;
};

struct EdgeDetail {
    std::string from;
    std::string to;
    std::vector<EntityUse> entities;
};

struct ModuleLink {
    std::string module;
    std::uint32_t entities = 0;
};

struct FileLinks {
    std::string path;
    bool source = false;
    std::vector<ModuleLink> uses;
    std::vector<ModuleLink> used_by;
};

/// The headers of a module other modules use alike: a module whose groups
/// have unrelated consumers splits along them.
struct ConsumerGroup {
    std::vector<std::string> consumers;
    std::vector<std::string> headers;

    /// Other modules these headers name.
    std::vector<std::string> depends_on;
};

struct ModuleDetail {
    std::string name;
    std::vector<FileLinks> files;
    std::vector<ConsumerGroup> groups;
    std::vector<std::string> internal_headers;
};

struct NamedUses {
    std::string module;
    std::vector<std::string> entities;
};

struct AnnotationValue {
    std::string name;
    std::string unit;
    double value = 0;
};

struct FileDetail {
    std::string path;
    std::string module;
    bool source = false;
    std::vector<AnnotationValue> annotations;
    std::vector<NamedUses> uses;
    std::vector<NamedUses> used_by;
    std::vector<ModuleLink> affinity;
};

/// A header the index holds several row variants of. Rows are names and
/// positions, so a variant count of one does not prove the header reads
/// the same everywhere (an initializer may still differ).
struct VariantHeader {
    std::string path;
    std::uint32_t variants = 0;
    std::uint32_t units = 0;

    /// What tells the variants apart: names only some of them carry.
    std::vector<std::string> unstable;
    bool declarations_differ = false;
};

struct InternalUse {
    std::string entity;
    std::string owner;

    /// "static" or "anonymous namespace".
    std::string reason;
    std::vector<std::string> users;

    /// The owner names it beyond its declaration: whatever inline function
    /// or template does so exposes it once exported.
    bool exposed_in_owner = false;
};

struct RedeclarationEntry {
    std::string entity;
    std::string owner;
    std::string file;
    std::uint32_t line = 0;

    bool friend_declaration = false;

    /// The file never names it beyond declaring it: deleting the
    /// declaration resolves the entry.
    bool unused = false;

    /// The edge it implies lies on a cycle: replacing it with an import of
    /// the owner's module would close one.
    bool on_cycle = false;
};

struct Located {
    std::string name;
    std::string related;
    std::string file;
    std::uint32_t line = 0;
};

struct ForeignEntry {
    std::string name;
    std::string owner;
    std::string file;
    std::uint32_t line = 0;
    bool friend_declaration = false;
};

struct DuplicateEntry {
    std::string entity;
    std::vector<std::string> files;
};

struct ConfiguringEntry {
    std::string macro;
    std::string definition;
    std::vector<std::string> readers;
};

struct ProviderEntry {
    std::string path;
    std::vector<std::string> specializes;
};

struct ContextMacroEntry {
    std::string macro;
    std::string definition;
    std::string file;
    std::uint32_t line = 0;
};

struct Obstacles {
    std::vector<VariantHeader> variant_headers;

    /// Macros a header tests in a preprocessor condition without including
    /// their definition: compiled alone it silently reads differently.
    std::vector<ContextMacroEntry> context_macros;

    /// Macros a header expands without including their definition: compiled
    /// alone it fails until the defining header is included, a mechanical
    /// fix.
    std::vector<ContextMacroEntry> borrowed_macros;
    std::vector<InternalUse> internal_uses;
    std::vector<RedeclarationEntry> cross_module_redeclarations;
    std::vector<ForeignEntry> foreign_declarations;

    /// An entity declared in one module's header and defined in another
    /// module's source.
    std::vector<Located> split_definitions;

    std::vector<DuplicateEntry> duplicate_definitions;
    std::vector<ConfiguringEntry> configuring_macros;

    /// Headers specializing out-of-scope templates, which instantiations
    /// use without naming: keep them where every instantiating unit imports
    /// them.
    std::vector<ProviderEntry> implicit_providers;

    /// Specializations of a scoped template in another module; legal, but
    /// reachable only where the specializing module is imported.
    std::vector<Located> specializations;
};

/// A macro other files use. Imports carry no macros, so every one of these
/// reaches its users through a textual macro header, its own module's files
/// included.
struct MacroUse {
    std::string macro;
    std::string definition;

    /// Per module, the files using it.
    std::vector<ModuleLink> users;
};

/// The answer of each view, shaped for an agent reading JSON: counts first,
/// names to drill into, long lists capped by `limit`.
struct Report {
    const Facts& facts;
    const Partition& partition;
    const Annotations& annotations;

    Overview overview(std::uint32_t limit) const;

    std::expected<EdgeDetail, std::string> edge(llvm::StringRef from, llvm::StringRef to) const;

    std::expected<ModuleDetail, std::string> module(llvm::StringRef name) const;

    std::expected<FileDetail, std::string> file(llvm::StringRef path) const;

    Obstacles obstacles() const;

    /// Macros used outside the file defining them, the input of the
    /// macro-header grouping.
    std::vector<MacroUse> macros() const;

    std::vector<Impact> impact() const;
};

}  // namespace clice::analysis
