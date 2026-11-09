/// What a wrapper module over each module's headers needs: the modules it
/// imports, the headers its global module fragment includes, the names it
/// exports and the macros its importers replay.

module clice:analysis.interface;

import :analysis.facts;
import :analysis.partition;
import :support.prelude;

namespace clice::analysis {

struct InterfaceAlias {
    std::string name;
    std::string target;
};

/// How an interface exports a name.
enum class ExportForm : std::uint8_t {
    /// A using-declaration of the name.
    Name,
    /// A using-enum-declaration of the unscoped enum `name`: its
    /// enumerators at once.
    Enum,
    /// A using-enum-declaration of the enum without a name the enumerator
    /// `name` belongs to, named by the enumerator's type.
    EnumOf,
};

struct InterfaceExport {
    std::string name;

    /// The header declaring it, one entry per header.
    std::string file;

    /// Whether files of other modules name it, as opposed to it merely
    /// being on the surface their units enter.
    bool used = false;

    ExportForm form = ExportForm::Name;
};

/// A header as files of other modules include it.
struct InterfaceHeader {
    std::string file;

    /// The names their directives spell it by, `llvm/ADT/StringRef.h`:
    /// where a directory first on the include path shadows it. A quoted name
    /// the includer's own directory resolves is none: that directory comes
    /// first.
    std::vector<std::string> names;

    /// The directive's operand to include it by: the name files of other
    /// modules spell it by, an angled one first, else the name its own
    /// module's files spell it by, else its quoted path. A system header
    /// resolves by the search path alone: <stdio.h> by its path would
    /// #include_next itself.
    std::string include;

    /// For a kept module, one name a file of another module reaches only
    /// through emptied headers, and that file.
    std::string because;
};

struct InterfaceMacro {
    std::string name;

    /// The module and file defining it.
    std::string module;
    std::string file;
    std::string directive;
};

/// A TU-local function or variable a module's headers use that headers of
/// other wrapped modules use too.
struct InterfaceSharedLocal {
    std::string name;

    /// The header declaring it.
    std::string file;

    /// The other wrapped modules whose headers use it.
    std::vector<std::string> modules;
};

/// A kept module's header holding an internal-linkage entity files of other
/// modules name, which no unit can export.
struct InterfaceLocal {
    std::string file;

    /// The header of `textual` those files include for it.
    InterfaceHeader entry;
};

/// A module as an interface unit wrapping its headers whole, as a
/// third-party library's module does: the global module fragment imports
/// what its headers need and includes its entries, the purview exports its
/// namespace-scope names, and its macros reach the importers through a
/// macro header.
struct Interface {
    std::string module;

    /// Modules its headers name entities or macros of, or include headers
    /// of.
    std::vector<std::string> imports;

    /// Its headers files of other modules include: what the fragment
    /// includes, and what importers see emptied.
    std::vector<InterfaceHeader> entries;

    /// Every namespace-scope name its headers provide, internal-linkage ones
    /// apart: what the purview exports, the names downstream modules'
    /// headers need included.
    std::vector<InterfaceExport> exports;
    std::vector<InterfaceAlias> aliases;

    /// Headers providing an internal-linkage entity other modules name, or
    /// a non-constant static variable to each file of theirs including
    /// them: no interface exports those, so the headers stay textual
    /// wherever the module is imported. For a module kept headers, the
    /// headers to include for what files of other modules name but reach
    /// only through emptied headers, no imported module providing it.
    std::vector<InterfaceHeader> textual;

    /// For a kept module, the names of its `textual` headers files of other
    /// modules name: what its own unit exports over those headers.
    std::vector<InterfaceExport> kept_exports;

    /// For a kept module, its headers holding an internal-linkage entity
    /// files of other modules name.
    std::vector<InterfaceLocal> locals;

    /// Textual headers of other wrapped modules its headers name entities
    /// of: what the fragment includes beside the imports.
    std::vector<InterfaceHeader> textual_uses;

    /// TU-local entities its headers share with other wrapped modules, from
    /// a header their fragments include alike (`<emmintrin.h>`'s intrinsics):
    /// clang mangles internal linkage alike across modules, so an importer
    /// of both defines each twice under one name. Such modules have to be
    /// one.
    std::vector<InterfaceSharedLocal> shared_locals;

    /// Macros other modules use and those their directives expand, in the
    /// order their files define them.
    std::vector<InterfaceMacro> macros;

    /// Macros of other modules its headers read, a switch the program
    /// defines ahead of a library's include among them: the fragment
    /// defines the ones of modules it does not import ahead of its entries.
    std::vector<InterfaceMacro> reads;
};

/// Every module's interface, by module.
std::vector<Interface> interfaces(const Facts& facts, const Partition& partition);

}  // namespace clice::analysis
