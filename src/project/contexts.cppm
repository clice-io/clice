module clice:project.contexts;

import :project.command_resolver;
import :project.project;
import :support.prelude;

namespace clice {

/// One context the listing offers a file: a host and the command it lends,
/// at one of the places its compile enters the header when there are
/// several, or one of the file's own entries.
struct ListedContext {
    /// The host's file name, or the flags setting the own entry apart; the
    /// lent command's flags in brackets when the host lends several, and the
    /// place, from #1, when it enters the header at several.
    std::string label;

    /// The host's path, or the own entry's directory, as shown.
    std::string description;

    /// The host, or the file itself for one of its own entries.
    Fid file;

    /// The place among those the host's compile enters the header at.
    std::optional<std::uint32_t> occurrence;

    /// The command's hash when the item stands for one of several: one of
    /// the commands the host lends, or an own entry.
    std::optional<std::string> command_hash;

    /// Whether two items stand for the same context.
    bool same(const ListedContext& other) const {
        return file == other.file && occurrence == other.occurrence &&
               command_hash == other.command_hash;
    }
};

/// The contexts a file can compile in, in this project: its hosts, ranked,
/// each with the commands it lends and the places it enters the header at,
/// then the file's own database entries. Contexts compiling the same under
/// one command list once.
std::vector<ListedContext> list_contexts(Project& project, Fid file);

/// The listing's item for the context the file compiles in: `choice`, its
/// user's active context choice, else what `resolution` picked — the file's
/// own entry, else the host it borrows a command from. None when the file
/// compiles under a rule's default command or a lender's.
std::optional<ListedContext> current_context(Project& project,
                                             Fid file,
                                             const ContextChoice* choice,
                                             const Resolution& resolution);

/// The context choice of the item `listed` names (switchContext's
/// parameters): a host at an occurrence, under one of its commands when it
/// names one, or one of the file's own entries.
ContextChoice choice_of(Project& project,
                        Fid file,
                        Fid listed,
                        std::optional<std::uint32_t> occurrence,
                        const std::optional<std::string>& command_hash);

/// Whether `choice` holds against the build and the include trees: its host
/// still compiles and enters the file, at its occurrence when it names one,
/// and its command still exists — an own entry's, or the host's it names.
/// A host whose include tree no run took yet holds.
bool holds(Project& project, Fid file, const ContextChoice& choice);

/// The choices encoded as the project's database carries them, files named
/// portably.
std::string serialize_choices(Project& project,
                              llvm::ArrayRef<std::pair<Fid, const ContextChoice*>> choices);

/// The choices `bytes` encodes; none from bytes that do not parse.
std::vector<std::pair<Fid, ContextChoice>> parse_choices(Project& project, llvm::StringRef bytes);

}  // namespace clice
