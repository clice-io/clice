module clice:project.hosting;

import :command.table;
import :compile.compilation;
import :feature.diagnostics;
import :index.symbol;
import :project.build;
import :project.memo;
import :support.prelude;
import :syntax.header_context;
import :vfs.file_table;

namespace clice {

/// A translation unit standing in for a header, with the include chain
/// from it to the header.
struct Host {
    Fid file;

    /// From the unit, or from a file its command forces in where the
    /// compile enters the header through one, to the header.
    std::vector<Fid> chain;

    /// The line of the directive each chain file but the header enters the
    /// next one with.
    llvm::SmallVector<std::uint32_t> lines;
};

/// A unit and one of its own commands (CommandTable::command_hash of it,
/// rules applied): what an include tree is the tree of.
using UnitCommand = std::pair<Fid, std::uint64_t>;

/// The include tree a preprocess run of a unit took: where its compile
/// enters each file.
struct PreprocessedTree {
    /// The version of the unit the run read; invalid when the run failed:
    /// the command cannot preprocess its unit, which then hosts nothing.
    VersionID root;

    std::vector<index::IncludeNode> nodes;
};

/// An include tree a header's resolution needs and no run took: the unit's
/// command to preprocess it under.
struct TreeRequest {
    UnitCommand key;

    CommandRef command;
};

/// What a self-containment check found of a header under the command it
/// borrows: whether it compiles alone, or misses names its includer
/// provides (missing_context_errors) and needs the includer's context.
struct SelfContainment {
    bool self_contained = false;

    /// The header's text the check compiled.
    VersionID text;

    /// The header's disk content when the check ran, which an open
    /// header's result holds while: its buffer may differ, by typing or
    /// from the start.
    VersionID disk;
};

/// A header and the command it borrows (CommandTable::command_hash of it,
/// the rules of both files applied).
using SelfContainmentKey = std::pair<Fid, std::uint64_t>;

/// The context of a header under a host: what it was synthesized for, and
/// the files restoring the includer's preprocessor state.
struct HeaderContext {
    Fid host;

    std::uint64_t command = 0;

    std::vector<Fid> chain;

    llvm::SmallVector<std::uint32_t> lines;

    /// Null when the synthesis could not follow the chain under the host's
    /// command.
    std::shared_ptr<const SynthesizedContext> synthesized;
};

/// The commands the members can lend — every command of every member but
/// the builtin ones, by member path — and the header search directories
/// they cover, by identity, as indexes into `commands`. Rebuilt when
/// Project::commands_epoch moves, so a resolution scans no member.
struct BorrowableCommands {
    struct Command {
        Fid member;

        /// The command as the member compiles under it.
        Candidate command;

        /// What the command compiles its member as.
        clang::driver::types::ID language;
    };

    llvm::SmallVector<Command> commands;
    llvm::StringMap<llvm::SmallVector<std::uint32_t>> search_dirs;
    std::uint64_t epoch = 0;
};

/// The hosts ranked for a header (ranked_hosts), and what they were ranked
/// against: the include graph, the build's commands and the index's
/// manifests, by their generations.
struct Ranking {
    llvm::SmallVector<Fid> hosts;

    std::array<std::uint64_t, 3> generations = {};
};

/// What clice knows of the headers that borrow a unit's command, the
/// answers every process reads alike: the CLI and the server resolve a
/// header the same way from the same answers. The self-containment checks,
/// each a compile, ride the precompiled records; the rest a process learns
/// again.
struct Hosting {
    /// A header with thousands of hosts takes milliseconds to rank, and
    /// every resolution of it asks.
    llvm::DenseMap<Fid, Ranking> rankings;

    /// The include trees preprocess runs took, where the index holds no
    /// manifest that tells (include_chains).
    Memo<UnitCommand, PreprocessedTree> trees;

    /// What the self-containment checks found.
    Memo<SelfContainmentKey, SelfContainment> self_containment;

    /// The latest context synthesized for each header, a failed synthesis
    /// included.
    Memo<Fid, HeaderContext> contexts;

    /// What a file without a command can borrow (see borrowed_command).
    BorrowableCommands borrowable;
};

/// The language a command compiles its unit as — a `-x` in the entry or a
/// rule's append included, else the unit's suffix as its driver reads it
/// (`g++` takes a `.c` as C++).
clang::driver::types::ID language_of(const CommandRef& command);

/// Whether the file at `path` can be part of the translation unit `unit`
/// compiled as `language`. A source only in its own, in the one the
/// unit's command gives the unit's own suffix (`g++` takes a `.c` as
/// C++), or across C++ and its modules: a `.cpp` compiled as CUDA or a
/// `.m` as C is not the file. A header has latitude: a `.h` fits any, a
/// C++ header every language built on C++ (Objective-C++, CUDA, HIP), a
/// `.cuh` CUDA.
bool compatible(llvm::StringRef path, llvm::StringRef unit, clang::driver::types::ID language);

/// How far `other` sits from `path` in the directory tree: how much of
/// `path` lies past the directories the two share, then how many
/// directories deeper `other` goes below them.
std::pair<std::size_t, std::size_t> tree_distance(llvm::StringRef path, llvm::StringRef other);

/// What finding the host of a header found.
struct HostSearch {
    std::optional<Host> host;

    /// The place among the ones the host's compile enters the header at.
    std::uint32_t occurrence = 0;

    /// The search stopped at a host whose include tree no run took: the
    /// hosts ranked after it come only once it is taken.
    std::optional<TreeRequest> missing;
};

/// How many errors name what an includer would have provided: an
/// undeclared name, a missing member or template of a namespace, a call to
/// an undeclared C function, an #error guarding against a standalone
/// compile, a conditional the includer closes. A typo raises the same
/// ones; the self-containment check runs when a header opens or its
/// inputs change, never per edit, so one costs at most a compile under
/// the includer's context.
std::size_t missing_context_errors(llvm::ArrayRef<feature::FileDiagnostic> diagnostics);

}  // namespace clice
