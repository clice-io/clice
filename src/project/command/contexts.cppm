module clice:project.contexts;

import :support.prelude;
import :vfs.ids;

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
};

}  // namespace clice
