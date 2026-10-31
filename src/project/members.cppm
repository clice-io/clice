module clice:project.members;

import :project.command;
import :settings.rules;
import :support.prelude;
import :vfs.ids;
import :vfs.path;

namespace clice {

/// The files the user opened or saved that the build does not declare
/// (Project::record_provisional): recorded until an entry or a rule covers
/// them, persisted with the index, and each a member while it has the
/// command it borrows (Project::refresh_provisional).
class ProvisionalMembers {
public:
    /// False when `file` is recorded already.
    bool record(Fid file) {
        if(!table.try_emplace(file).second) {
            return false;
        }
        generation += 1;
        return true;
    }

    /// Drop the record of `file`, and its command.
    void forget(Fid file) {
        if(table.erase(file)) {
            generation += 1;
        }
    }

    bool recorded(Fid file) const {
        return table.contains(file);
    }

    /// The command `file` compiles under as a member; null while it is none.
    const Candidate* command(Fid file) const {
        auto it = table.find(file);
        return it != table.end() && it->second ? &*it->second : nullptr;
    }

    /// Set the command a recorded file compiles under, nullopt while it is
    /// none; returns the one it had.
    std::optional<Candidate> set_command(Fid file, std::optional<Candidate> command) {
        auto it = table.find(file);
        assert(it != table.end() && "only a recorded file is a provisional member");
        return std::exchange(it->second, std::move(command));
    }

    /// The recorded files, unordered.
    auto files() const {
        return views::transform(table, [](const auto& entry) { return entry.first; });
    }

    /// Moves whenever the recorded files change, so persistence can tell.
    std::uint64_t changes() const {
        return generation;
    }

private:
    llvm::DenseMap<Fid, std::optional<Candidate>> table;

    std::uint64_t generation = 0;
};

/// Every file under the walk's roots, version control metadata, the cache
/// directory and the projects nested in a root skipped. File system work
/// only, so it can run off the event loop: a rule without patterns has the
/// whole project walked.
std::vector<CanonicalPath> walk_sources(const SourceWalk& walk);

}  // namespace clice
