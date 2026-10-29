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

struct Project;
struct Selection;

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

/// What a self-containment trial found of a header under the command it
/// borrows: whether it compiles alone, or misses names its includer
/// provides (missing_context_errors) and needs the includer's context.
struct Trial {
    bool self_contained = false;

    /// The header's text the trial compiled.
    VersionID text;

    /// The header's disk content when the trial ran, which an open
    /// header's result holds while: its buffer may differ, by typing or
    /// from the start.
    VersionID disk;
};

/// A header and the command it borrows (CommandTable::command_hash of it,
/// the rules of both files applied).
using TrialKey = std::pair<Fid, std::uint64_t>;

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

/// A unit and the one of its base commands that a file outside the build
/// borrows.
struct Lender {
    Fid unit;
    CommandID command;
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

/// The hosts ranked for a header (ranked_hosts), and what they were ranked
/// against: the include graph, the build's commands and the index's
/// manifests, by their generations.
struct Ranking {
    llvm::SmallVector<Fid> hosts;

    std::array<std::uint64_t, 3> generations = {};
};

/// What clice knows of the headers that borrow a unit's command, the
/// answers every process reads alike: the CLI and the server resolve a
/// header the same way from the same answers. The trials, each a compile,
/// ride the artifact records; the rest a process learns again.
struct Hosting {
    /// A header with thousands of hosts takes milliseconds to rank, and
    /// every resolution of it asks.
    llvm::DenseMap<Fid, Ranking> rankings;

    /// The include trees preprocess runs took, where the index holds no
    /// manifest that tells (include_chains).
    Memo<UnitCommand, PreprocessedTree> trees;

    /// What the self-containment trials found.
    Memo<TrialKey, Trial> trials;

    /// The latest context synthesized for each header, a failed synthesis
    /// included.
    Memo<Fid, HeaderContext> contexts;

    /// What a file without a command can borrow (see command_lender).
    LenderIndex lenders;
};

/// Every place the compile of the unit `unit` names, under the command it
/// names, enters `header`, in the order it does, by the unit's include
/// tree: its index manifest's for the command the index runs under, its
/// first, else the one a preprocess run took. Empty when the compile never enters the header.
/// Nullopt when no tree can tell: the manifest cannot — the unit or a file on its way changed
/// since, a file appeared where the compile looked and found none, or a forced file a precompiled
/// header held hides the way in — and no run took one since the inputs it read last changed.
std::optional<llvm::SmallVector<Host>> include_chains(Project& project,
                                                      const UnitCommand& unit,
                                                      Fid header);

/// The unit's own command `candidate`, rules applied, and its key.
TreeRequest tree_request(Project& project, Fid unit, const Candidate& candidate);

/// The translation units that can stand in for `header` — the units of the
/// build whose compiles read it by the lexical scan, also through a file
/// their commands force in, or by the entries their indexed compiles gave
/// it, that compile in a language the header can be part of (a `.h` in
/// any, a `.hpp` in C++ and the languages built on it, a `.cuh` only in
/// CUDA) and write no precompiled header — best first: units whose entries
/// come from the databases the header's own rules name, then the unit
/// sharing the header's stem, its directory, and path proximity.
llvm::SmallVector<Fid> ranked_hosts(Project& project, Fid header);

/// The commands of `host` that compile `header` in a language it can be
/// part of, in the host's order: what the header may compile under, its
/// first the default. Empty when the host cannot stand in for it.
llvm::SmallVector<Candidate, 2> host_commands(Project& project, Fid header, Fid host);

/// What finding the host of a header found.
struct HostSearch {
    std::optional<Host> host;

    /// The place among the ones the host's compile enters the header at.
    std::uint32_t occurrence = 0;

    /// The search stopped at a host whose include tree no run took: the
    /// hosts ranked after it come only once it is taken.
    std::optional<TreeRequest> missing;
};

/// The host a header compiles under: the pinned one while its compile
/// enters the header at the pinned place, else the first ranked one whose
/// compile enters it, where it first does.
HostSearch find_host(Project& project, Fid header, const Selection* pin);

/// The host `header` compiles under with nothing pinned, or waits for the
/// include tree of; invalid for a file no unit includes. A header compiled
/// under its own entry has one too: the source its definitions belong in.
Fid host_of(Project& project, Fid header);

/// The places the compile of `host` under its first command fitting
/// `header` enters it; nullopt when no tree tells.
std::optional<std::uint32_t> count_occurrences(Project& project, Fid host, Fid header);

/// The self-containment trial's result for `key`; nullopt when no trial
/// stands for it. `open`: the header is a document the user edits, whose
/// text moves with every keystroke: a result holds while its disk content
/// stays as it was when the trial ran, as typing never runs the trial
/// again; otherwise while the disk holds the text the trial compiled.
std::optional<bool> self_contained(Project& project, const TrialKey& key, bool open);

/// How many errors name what an includer would have provided: an
/// undeclared name, a missing member or template of a namespace, a call to
/// an undeclared C function, an #error guarding against a standalone
/// compile, a conditional the includer closes. A typo raises the same
/// ones; the self-containment trial runs when a header opens or its
/// inputs change, never per edit, so one costs at most a compile under
/// the includer's context.
std::size_t missing_context_errors(llvm::ArrayRef<feature::FileDiagnostic> diagnostics);

/// Remember what the trial of `key` found: `diagnostics`, what the header's
/// compile without its includer's context raised over `text`, having read
/// `trace`.
void commit_trial(Project& project,
                  const TrialKey& key,
                  llvm::ArrayRef<feature::FileDiagnostic> diagnostics,
                  VersionID text,
                  DepsSnapshot trace);

/// The header's buffer was saved as `text`: the trial of `key`, when one
/// found it self-contained and `leans` says the compile of that text shows
/// what an includer provides, is forgotten, to run again; any other result
/// holds for the saved text. Whether it was forgotten.
bool saved_trial(Project& project, const TrialKey& key, VersionID text, bool leans);

/// Remember the tree a preprocess run under `request` took: the files it
/// read (`paths` with the hashes of the bytes read, the unit's own last),
/// its include nodes over them, and every file it read or looked for
/// (`deps`), the run started at `build_at`.
void commit_tree(Project& project,
                 const TreeRequest& request,
                 llvm::ArrayRef<std::string> paths,
                 llvm::ArrayRef<std::uint64_t> hashes,
                 std::vector<index::IncludeNode> nodes,
                 llvm::ArrayRef<DepFile> deps,
                 std::int64_t build_at);

/// Remember that the run under `request` failed: the unit's command cannot
/// preprocess it as the unit stands now.
void commit_failed_tree(Project& project, const TreeRequest& request);

/// Take the tree `request` names with a preprocess run in this process, as
/// a worker takes it for the server: the command line's way, which runs no
/// workers.
void take_tree(Project& project, const TreeRequest& request);

/// The context `header` compiles in under `host`, its command `command`
/// borrowed, with the files it embeds: the includer's state up to where
/// the compile enters it, read from disk — never from open buffers: open
/// files are no dependency of other files. Null when the synthesis cannot
/// follow the chain under the command; remembered either way while the
/// chain files hold.
Memo<Fid, HeaderContext>::Entry
    synthesize(Project& project, Fid header, const Host& host, const CommandRef& command);

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

}  // namespace clice
