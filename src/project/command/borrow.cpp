module clice;

import :command.file_kind;
import :command.search_config;
import :project.command;
import :project.project;
import :vfs.path;

namespace clice {

const static BorrowableCommands& borrowable_commands(Project& project) {
    auto& index = project.hosting.borrowable;
    if(index.epoch == project.commands_epoch) {
        return index;
    }
    index.commands.clear();
    index.search_dirs.clear();
    auto members = project.members();
    ranges::sort(members, {}, [&](Fid unit) { return project.file_table.resolve(unit); });
    llvm::StringMap<CanonicalPath> identities;
    for(auto member: members) {
        // A member a rule covers with a default command that is no compile
        // command has none; giving the builtin one would only name a guess
        // borrowed.
        for(auto& candidate: project.candidates(member)) {
            if(candidate.source == CommandSource::Fallback) {
                continue;
            }
            auto ref = project.tree_request(member, candidate).command;
            auto position = static_cast<std::uint32_t>(index.commands.size());
            index.commands.push_back({
                .member = member,
                .command = candidate,
                .language = language_of(ref),
            });
            for(auto& search_dir: project.toolchain.search_config(ref).dirs) {
                auto [it, inserted] = identities.try_emplace(search_dir.path);
                if(inserted) {
                    it->second = CanonicalPath(Spelling::absolute(search_dir.path));
                }
                index.search_dirs[it->second].push_back(position);
            }
        }
    }
    index.epoch = project.commands_epoch;
    return index;
}

std::optional<Candidate> Project::borrowed_command(Fid file, bool provisional) {
    auto& files = file_table;
    auto path = files.resolve(file);
    bool header = is_header_path(path);
    auto dir = path::parent(path);
    auto stem = path::stem(path);
    auto& index = borrowable_commands(*this);
    auto fits = [&](const BorrowableCommands::Command& command) {
        return command.member != file &&
               (provisional || command.command.source != CommandSource::Borrowed) &&
               compatible(path, files.resolve(command.member), command.language);
    };

    using Command = BorrowableCommands::Command;
    auto borrowed = [](const Command& command) {
        return command.command.passed_on(command.member, CommandSource::Borrowed);
    };
    // Every member with its first command of the family, in path order.
    llvm::SmallVector<const Command*> units;
    for(auto& command: index.commands) {
        if(fits(command) && (units.empty() || units.back()->member != command.member)) {
            units.push_back(&command);
        }
    }
    if(units.empty()) {
        return std::nullopt;
    }
    auto unit_path = [&](const Command* command) {
        return files.resolve(command->member);
    };

    auto siblings = ranges::to<llvm::SmallVector<const Command*>>(
        views::filter(units, [&](const Command* command) {
            return path::parent(unit_path(command)) == dir;
        }));
    if(!siblings.empty()) {
        return borrowed(**ranges::min_element(siblings, {}, [&](const Command* command) {
            return std::tuple(path::stem(unit_path(command)) != stem, unit_path(command));
        }));
    }

    // A header some command's header search reaches: that unit's code
    // finds it by that path, so the command is the one the header is
    // written for. Nearest directory first, by path and command within.
    if(header) {
        for(auto ancestor: path::ancestors(dir)) {
            auto it = index.search_dirs.find(ancestor);
            if(it == index.search_dirs.end()) {
                continue;
            }
            for(auto position: it->second) {
                if(fits(index.commands[position])) {
                    return borrowed(index.commands[position]);
                }
            }
        }
    }

    // The closest unit in the directory tree, then by name.
    return borrowed(**ranges::min_element(units, {}, [&](const Command* command) {
        return std::tuple(path::tree_distance(path, unit_path(command)), unit_path(command));
    }));
}

}  // namespace clice
