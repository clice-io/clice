/// Which module each scoped file belongs to: the partition a spec of globs,
/// moves and merges describes, and the names a module may take.

module clice:analysis.partition;

import :analysis.facts;
import :support.prelude;

namespace clice::analysis {

/// What a module is to the program a partition wraps.
enum class ModuleKind : std::uint8_t {
    /// A directory of the program no glob covers: its headers stay.
    Program,

    /// Wrapped: files of other modules see its headers emptied and import
    /// a module interface over them.
    Wrapped,

    /// Kept headers beside the wrapped modules, as the C library beside
    /// `import std`.
    Textual,

    /// An existing module interface stands for it, as libc++'s std: its
    /// headers are emptied, and its files compile as they are there.
    External,
};

/// An assignment of every scoped file to a module.
struct Partition {
    std::vector<std::string> modules;
    std::vector<std::uint32_t> module_of;

    std::vector<ModuleKind> kinds;

    /// Per module: the names of its headers another module's interface
    /// exports, as `std.compat` exports the C library's.
    std::vector<llvm::StringSet<>> provides;

    /// Per module: the primary interface unit of a program module rewritten
    /// into a named module, relative to the project root; empty for one whose files
    /// stay as they are.
    std::vector<std::string> primaries;

    /// Whether files of other modules see the module's headers emptied.
    bool emptied(std::uint32_t module) const {
        return kinds[module] == ModuleKind::Wrapped || kinds[module] == ModuleKind::External;
    }

    /// The module so named; `modules.size()` for none.
    std::uint32_t module_named(llvm::StringRef name) const {
        return static_cast<std::uint32_t>(ranges::find(modules, name) - modules.begin());
    }
};

struct PartitionSpec {
    /// Directory segments that name a file's default module; 0 for its
    /// whole directory.
    std::uint32_t depth = 0;

    struct Module {
        std::string name;

        /// Globs over paths relative to the project root, absolute ones
        /// outside the project.
        std::vector<std::string> files;

        ModuleKind kind = ModuleKind::Wrapped;

        /// The names of its headers another module's interface exports.
        llvm::StringSet<> provides;

        /// A program module whose files are rewritten into the units of a
        /// named module, its primary interface unit at `primary` (default:
        /// `module.cppm` in the deepest directory holding its headers).
        bool rewrite = false;
        std::string primary;
    };

    /// Modules covered by globs, first match wins; uncovered files keep their
    /// directory module. A name listed again adds globs, and its kind and
    /// provided names.
    std::vector<Module> modules;

    /// `path=module` reassignments, applied after the globs.
    std::vector<std::string> moves;

    /// `a+b+c`: b's and c's files join a.
    std::vector<std::string> merges;
};

std::expected<Partition, std::string> partition(const Facts& facts, const PartitionSpec& spec);

/// Whether `word` is a C++23 keyword rather than an identifier.
bool is_keyword(llvm::StringRef word);

/// A C++ module name: identifiers joined by dots, none a keyword, `module` or
/// `import`, which also keeps the files named after it inside the output
/// directory.
bool is_module_name(llvm::StringRef name);

}  // namespace clice::analysis
