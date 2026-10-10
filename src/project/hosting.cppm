module;

#include "modules/prelude.h"

module clice:project.hosting;

import :command.command;
import :index.include_tree;
import :project.build;
import :vfs.file_table;

namespace clice {

struct Project;

/// A translation unit standing in for a header, with the include chain
/// from it to the header.
struct Host {
    Fid file;
    std::vector<Fid> chain;

    /// The line of the directive each chain file but the header enters the
    /// next one with, when the unit's include tree says where its compile
    /// enters the header; empty for a lexical chain.
    llvm::SmallVector<std::uint32_t> lines;

    /// Whether the chain is the lexical scan's for want of an include tree
    /// that could tell: its directives are ones the compile may never enter.
    bool lexical = false;
};

/// An include tree over file versions a preprocess run took of a unit
/// (Project::include_trees), beside the ones index manifests carry: rooted
/// at the version of the unit it read, invalid while no run succeeded.
struct HostTree {
    VersionID root;
    std::vector<index::IncludeNode> nodes;

    /// Project::commands_epoch of the run the tree came from: it serves
    /// the commands it ran under.
    std::uint64_t commands_epoch = 0;

    /// Project::commands_epoch and context_epoch when the last run, failed
    /// or not, started, counting a taken tree's own bump of the latter: one
    /// is not repeated before either moves.
    std::pair<std::uint64_t, std::uint64_t> last_run;
};

/// Every place the compile of `host` enters `header`, in the order it
/// does, by the unit's include tree — its index manifest's, else the one
/// a preprocess run left. Empty when the compile never enters the header.
/// Nullopt when no tree can tell: none is known, or the unit or a file on
/// the way changed since — for a header the tree never enters, a file of
/// the lexical chain to it, which may have gained the include, or the
/// header itself, appearing where the indexed compile found nothing. A
/// compile entering the header only through an include its command forces
/// in, which no cut of the unit's text reproduces, yields the lexical chain.
std::optional<llvm::SmallVector<Host>> enterings(Project& project, Fid host, Fid header);

/// How many times the compile of `host` enters `header`: by its include
/// tree, else as often as the direct includer on the lexical chain
/// includes it; 0 when the host does not include it.
std::uint32_t count_occurrences(Project& project, Fid host, Fid header);

/// The translation units that can stand in for `header` — its includers,
/// by the lexical scan or by the rows their indexed compiles gave it, that
/// the build compiles in a language the header can be part of (a `.h`
/// in any, a `.hpp` in C++ and the languages built on it, a `.cuh` only
/// in CUDA) — best first: units whose entries come from the databases
/// the header's own rules name, then the unit sharing the header's stem,
/// its directory, and path proximity.
llvm::SmallVector<Fid> ranked_hosts(Project& project, Fid header);

/// The commands of `host` that compile `header` in a language it can be
/// part of, in the host's order: what the header may compile under, its
/// first the default. Empty when the host cannot stand in for it.
llvm::SmallVector<Candidate, 2> host_commands(Project& project, Fid header, Fid host);

/// A unit and the one of its base commands that another file borrows.
struct Lender {
    Fid unit;
    ConfigID config;
};

/// The commands the build's units can lend — every command of every
/// member but the borrowed ones, by unit path — and the header search
/// directories they cover, by identity, as indexes into `commands`.
/// Rebuilt when Project::commands_epoch moves, so a resolution scans no
/// unit.
struct LenderIndex {
    struct Command {
        Lender lender;
        /// What the command compiles its unit as.
        clang::driver::types::ID language;
    };

    llvm::SmallVector<Command> commands;
    llvm::StringMap<llvm::SmallVector<std::uint32_t>> search_dirs;
    std::uint64_t epoch = 0;
};

/// The lender of a file with neither an entry nor a host
/// (CommandSource::Inferred), among the units the build declares in a
/// language the file can be part of (a `.h` matches any, a `.c` borrows
/// C++ only from a `.c` unit compiled as C++, a C++ source and a C++
/// module unit borrow from each other): one in the file's directory —
/// same stem first, then by name — with its first command; else, for a
/// header, the unit whose command's header search directories contain
/// it, nearest directory first, with that command; else the unit closest
/// by path. Nullopt when the build has no such unit.
std::optional<Lender> command_lender(Project& project, Fid file);

/// The host a header compiles under when nothing is pinned: the first
/// ranked one whose compile enters it, where it first does; the lexical
/// chain for a host no include tree tells about.
std::optional<Host> default_host(Project& project, Fid header);

}  // namespace clice
