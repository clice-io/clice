module clice:project.command_resolver;

import :command.table;
import :project.hosting;
import :project.project;
import :support.prelude;
import :vfs.file_system;

namespace clice {

/// What one resolution asks for beyond the file itself. The editor fields
/// are filled by EditorContext for editor-facing compiles of open files;
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

    /// Take the include trees the host search misses with preprocess runs
    /// in this process (take_tree): the command line's way, which runs no
    /// workers; the server takes them through its pool (IncludeTrees).
    bool preprocess = false;
};

/// Pick the candidate matching a chosen command (multi-configuration files
/// and hosts), defaulting to the build's first command. `paths` are the
/// files whose edits the published hash was computed with.
Candidate pick_chosen_command(Project& project,
                              Fid file,
                              llvm::ArrayRef<Candidate> candidates,
                              llvm::ArrayRef<CanonicalRef> paths,
                              llvm::StringRef language_path,
                              llvm::StringRef chosen_hash,
                              llvm::StringRef chosen_base);

/// `command`, one of `host`'s, as `host` lends it to `header`: edited by
/// the rules of both files and a run's extras, compiled as the host's
/// language.
CommandRef lent_command(Project& project,
                        Fid header,
                        Fid host,
                        CommandID command,
                        llvm::ArrayRef<std::string> extra_prepend = {},
                        llvm::ArrayRef<std::string> extra_append = {});

/// Of `host`'s commands fit for `header`, the chosen one, else the first;
/// nullopt when none fits.
std::optional<Candidate> chosen_host_command(Project& project,
                                             Fid header,
                                             Fid host,
                                             llvm::StringRef chosen_hash,
                                             llvm::StringRef chosen_base);

/// The command `host` lends `header`: chosen_host_command, as lent_command
/// lends it.
std::optional<CommandRef> host_command(Project& project,
                                       Fid header,
                                       Fid host,
                                       llvm::StringRef chosen_hash,
                                       llvm::StringRef chosen_base,
                                       llvm::ArrayRef<std::string> extra_prepend = {},
                                       llvm::ArrayRef<std::string> extra_append = {});

/// The dialect a file's text is lexed in, `choice` the user's context choice
/// for it and `host` the source whose context it is read in, either absent
/// for none: an explicit -x in the file's own database entry decides
/// outright; else the host's command; else C when the file is a C source
/// or every TU contributing its indexed entries is one, C++ else (mixed
/// inclusion favors the superset).
const clang::LangOptions&
    dialect(Project& project, Fid file, const ContextChoice* choice, Fid host);

/// How a file's command was resolved.
struct Resolution {
    CommandSource source = CommandSource::Fallback;

    /// The host whose command an IncludeGraph resolution borrowed;
    /// invalid for every other source.
    Fid host;

    /// The place among the ones the host's compile enters the header at
    /// that the header compiles as (see include_chains).
    std::uint32_t occurrence = 0;

    /// The unit whose command an Inferred resolution borrowed; invalid for
    /// every other source.
    Fid lender;

    /// The file the host's command forces in that the compile enters the
    /// header through, where the chain starts — the header itself when the
    /// command forces it in; invalid for a chain from the host's text. The
    /// header's compile leaves out that -include and the ones after it,
    /// which the host's compile enters after the header.
    Fid forced;

    /// The command selection behind the rendered arguments, for
    /// structured consumers (search config, language queries).
    CommandRef ref;

    /// The header context the arguments name (-include of its prefix),
    /// which exists only in memory: every compile under them must be
    /// served its files. Null when nothing was synthesized.
    std::shared_ptr<const SynthesizedContext> synthesized;

    /// The files the header's context rests on, at the versions read: the
    /// chain the host enters it by, whose text a synthesized context
    /// embeds. Inputs of every compile under the arguments that clang never
    /// reads from disk.
    DepsSnapshot embedded;

    /// The host whose includer context the header needed but whose include
    /// chain the synthesis could not follow: the command fell back past it.
    Fid unmatched_host;

    /// The include tree the header's host search needs and no run took: the
    /// command is that host's, without its includer's context, until it is
    /// taken.
    std::optional<TreeRequest> missing_tree;

    /// The self-containment trial deciding whether the header compiles in
    /// its includer's context; none for a header chosen to a host, which
    /// always does, or one its host's command forces in itself.
    std::optional<TrialKey> trial;

    /// What the trial found; nullopt while no result stands: a compile
    /// under the arguments, without the includer's context, is the trial.
    std::optional<bool> self_contained;

    /// Why the compiler of the command could not be queried, which leaves
    /// the arguments clang's own reading of the command; empty when it
    /// answered.
    std::string query_failure;

    /// The files on disk every compile under the arguments must not see.
    vfs::HiddenFiles hidden;
};

/// Composes a file's final compile command from the project on disk.
///
/// The build (`Build`) says what a file compiles as; hosting (`find_host`)
/// says which unit stands in for a header without a command of its own,
/// whether the header compiles alone, and the context restoring its
/// includer's preprocessor state when it does not. `resolve_command`
/// applies them in order — own entry, host, default command, lender,
/// builtin — then the rule edits and a run's extras; an editor request
/// layers the user's context choice on top.
class CommandResolver {
public:
    explicit CommandResolver(Project& project) : project(project) {}

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: the chosen host, the file's own command, a header
    /// context through the include graph, a default command, a lender and
    /// finally the builtin command — so it always succeeds. Emits a
    /// per-file decision log (tiers tried, tier hit, command hash).
    Resolution resolve_command(Fid path_id,
                               std::string& directory,
                               std::vector<std::string>& arguments,
                               const CommandRequest& request = {});

    /// Build::pch_unit for the command `resolution` and `request` gave
    /// `path_id`: edited by the rules of the files it was resolved for (a
    /// borrowing header's and its host's) and the request's extras.
    std::optional<CommandRef> pch_unit(Fid path_id,
                                       const Resolution& resolution,
                                       const CommandRequest& request = {});

private:
    /// resolve_command, the trees the host search misses left missing.
    Resolution resolve(Fid path_id,
                       std::string& directory,
                       std::vector<std::string>& arguments,
                       const CommandRequest& request);

    /// A header's command borrowed from its host, with the includer's
    /// context when the header needs it (in `resolution`); nullopt when no
    /// host stands in for it, or its context cannot be synthesized.
    std::optional<CommandRef> resolve_through_host(Fid path_id,
                                                   const CommandRequest& request,
                                                   Resolution& resolution);

private:
    Project& project;
};

}  // namespace clice
