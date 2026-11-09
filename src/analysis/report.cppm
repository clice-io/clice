/// The views `clice analyze modules` prints of the facts under a partition:
/// the overview, an edge, a module, a file, the obstacles, the macros and
/// the rebuild impact.

module clice:analysis.report;

import :analysis.annotation;
import :analysis.facts;
import :analysis.interface;
import :analysis.partition;
import :support.prelude;

namespace clice::analysis {

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
    /// imports, so an interface edit travels the whole importer closure;
    /// for an internal partition, the units entering it, which import it.
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

    /// Headers only their own module's sources use and include, directly
    /// or through other such headers: the generator makes them internal
    /// partitions, which import nothing into the module's interface and
    /// whose edits rebuild only the units including them.
    std::uint32_t internal_headers = 0;
    std::vector<std::string> deepest_chain;
    Totals totals;
    std::vector<Impact> hotspots;
    std::vector<MoveCandidate> moves;
    std::vector<SplitCandidate> splits;
    ObstacleCounts obstacles;
};

struct EntityUse {
    /// The locator naming it, as `clice query` and `--move-entity` take it:
    /// `#<hex>`, with `path:` the file declaring one of internal linkage.
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

/// A header the index holds several variants of. Index entries are names and
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

    /// A forward declaration the file never names beyond declaring it:
    /// deleting the declaration resolves the entry. A friend declaration,
    /// which grants access, is never unused.
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

    /// Every module's interface, or the named one's.
    std::expected<std::vector<Interface>, std::string> interface(llvm::StringRef name) const;
};

}  // namespace clice::analysis
