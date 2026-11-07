module clice:project.command;

import :command.table;
import :index.symbol;
import :project.memo;
import :support.prelude;
import :syntax.header_context;
import :vfs.file_system;
import :vfs.file_table;

namespace clice {

/// A command a file compiles under, before the rules edit it: one of its
/// database entries (Database); for a file without entries that a rule's
/// default command covers, that command (Default); for a provisional
/// member, the command it borrows (Borrowed), or the builtin one when it
/// borrows none (Fallback); for a header, one of its host's (Host).
struct Candidate {
    /// The command as the entry, the rule or the builtin gives it.
    CommandID base;

    CommandSource source;

    /// The files whose rules edit the command besides the file compiling
    /// under it, the one it is the command of last: none for the file's own;
    /// the unit it comes from for a borrowed command — the first of a chain of
    /// borrowings, whose command it is; for a header's, its host, after the
    /// files the host's own command was from.
    llvm::SmallVector<Fid, 2> rules_from;

    /// The file the command is the command of; invalid for the file's own.
    Fid from() const {
        return rules_from.empty() ? Fid{} : rules_from.back();
    }

    /// This command of `owner` as another file compiles under it, `as` Host
    /// or Borrowed. A header compiles in its host, under the host's rules
    /// too; a borrowed command stays the command of the unit it came from first.
    /// The builtin one stays Fallback: no build chose its compiler.
    Candidate passed_on(Fid owner, CommandSource as) const {
        Candidate candidate{
            .base = base,
            .source = source == CommandSource::Fallback ? source : as,
            .rules_from = rules_from,
        };
        if(as == CommandSource::Host || rules_from.empty()) {
            candidate.rules_from.push_back(owner);
        }
        return candidate;
    }

    friend bool operator==(const Candidate&, const Candidate&) = default;
};

/// The user's choice for a file in the editor (clice/switchContext): the
/// host to borrow a command from, or one of the file's own entries.
/// Persisted in the context choices; validated on didOpen.
struct ContextChoice {
    /// The host the header compiles in; invalid for one of the file's own
    /// entries.
    Fid host;

    /// The place among those the host's compile enters the header at; none
    /// for the first.
    std::optional<std::uint32_t> occurrence;

    /// The chosen command (CommandTable::command_hash_hex of its
    /// Candidate::base): one of the host's, or one of the file's own
    /// entries; empty for the host's first. Only the command as written
    /// identifies it: a rule's edit changes no choice, and a change of the
    /// command in its database, even of a flag a rule removes, ends it.
    std::string base;
};

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
/// provides (missing_context_diagnostics) and needs the includer's context.
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

/// The commands a file can borrow from the members — every command of every member but
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

/// What finding the host of a header found.
struct HostSearch {
    std::optional<Host> host;

    /// The place among the ones the host's compile enters the header at.
    std::uint32_t occurrence = 0;

    /// The search stopped at a host whose include tree no run took: the
    /// hosts ranked after it come only once it is taken.
    std::optional<TreeRequest> missing;
};

/// One context the listing offers a file: a host and one of its commands,
/// at one of the places its compile enters the header when there are
/// several, or one of the file's own entries.
struct ListedContext {
    /// The host's file name, or the flags setting the own entry apart; the
    /// command's flags in brackets when the host has several the header can
    /// compile under, and the place, from #1, when it enters the header at
    /// several.
    std::string label;

    /// The host's path, or the own entry's directory, as shown.
    std::string description;

    /// The host, or the file itself for one of its own entries.
    Fid file;

    /// The place among those the host's compile enters the header at.
    std::optional<std::uint32_t> occurrence;

    /// The command as written (ContextChoice::base) when the item stands for
    /// one of several: one of the host's commands, or an own entry.
    std::optional<std::string> command_hash;

    /// Whether `other` lists the same context: the same file, place and
    /// command, however each shows it.
    bool same_context(const ListedContext& other) const {
        return file == other.file && occurrence == other.occurrence &&
               command_hash == other.command_hash;
    }
};

/// What one resolution asks for beyond the file itself. The editor fields
/// are filled by the served project for editor-facing compiles of open
/// files (ServedProject::command_request);
/// a background resolution (indexing, batch runs, `clice inspect`) leaves
/// them empty and so depends on nothing but the project on disk.
struct CommandRequest {
    /// Per-run additions to the database driver command (a lint plan's
    /// clang-tool extra args), applied before toolchain resolution so the
    /// driver interprets them. Unlike rule appends they are never
    /// NVCC-translated; prepends land right after the binary name, appends
    /// win over rule appends.
    llvm::ArrayRef<std::string> extra_prepend;
    llvm::ArrayRef<std::string> extra_append;

    /// The user's context choice for the file (clice/switchContext).
    const ContextChoice* choice = nullptr;

    /// The file is open in the editor (see self_contained).
    bool open = false;
};

/// What a file compiles under, before anything is rendered
/// (Project::select_command).
struct SelectedCommand {
    /// The command the file compiles under before the rules edit it, and
    /// the files it is from.
    Candidate candidate;

    /// `candidate` as the file compiles under it — the rules' edits and a
    /// run's extras applied — and where it came from (CommandRef::source),
    /// for structured consumers (search config, language queries).
    CommandRef ref;

    /// The host a Host selection compiles the file in; invalid for every
    /// other source.
    Fid host;

    /// The place among the ones the host's compile enters the header at
    /// that the header compiles as (see include_chains).
    std::uint32_t occurrence = 0;

    /// The file the host's command forces in that the compile enters the
    /// header through, where the chain starts — the header itself when the
    /// command forces it in; invalid for a chain from the host's text. The
    /// header's compile leaves out that -include and the ones after it,
    /// which the host's compile enters after the header.
    Fid forced;

    /// Where the host's compile enters the header; none while its include
    /// tree is not taken, and for every other source.
    std::optional<Host> entry;

    /// The header compiles in its includer's context (render synthesizes
    /// it): a host the user chose, or one a self-containment check found it
    /// needs.
    bool in_context = false;

    /// The include tree the header's host search needs and no run took: the
    /// command is that host's, without its includer's context, until it is
    /// taken.
    std::optional<TreeRequest> missing_tree;

    /// The self-containment check deciding whether the header compiles in
    /// its includer's context; none for a header chosen to a host, which
    /// always does, or one its host's command forces in itself.
    std::optional<SelfContainmentKey> self_containment;

    /// What the check found; nullopt while no result stands: a compile
    /// under the arguments, without the includer's context, is the check.
    std::optional<bool> self_contained;

    /// The tiers tried, the one selecting last.
    llvm::SmallVector<llvm::StringRef, 6> tried;
};

/// A command as a compile runs it (Project::render).
struct Rendered {
    std::string directory;

    std::vector<std::string> arguments;

    /// The files on disk every compile under the arguments must not see.
    vfs::HiddenFiles hidden;

    /// Why the compiler of the command could not be queried, which leaves
    /// the arguments clang's own reading of the command; empty when it
    /// answered.
    std::string query_failure;
};

/// How a file's command was resolved (Project::resolve_command): what was
/// selected, rendered with the header's context.
struct Resolution : SelectedCommand, Rendered {
    /// The header context the arguments name (-include of its prefix),
    /// which exists only in memory: every compile under them must be
    /// served its files. Null when nothing was synthesized.
    std::shared_ptr<const SynthesizedContext> synthesized;

    /// The files the header's context rests on, at the versions read: the
    /// chain the host enters it by, whose text a synthesized context
    /// embeds. Inputs of every compile under the arguments that clang never
    /// reads from disk.
    Inputs inputs;

    /// The host whose includer context the header needed but whose include
    /// chain the synthesis could not follow: the header compiles under its
    /// command without it.
    Fid unmatched_host;
};

}  // namespace clice
