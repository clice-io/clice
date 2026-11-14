module clice;

import :command.file_kind;
import :index.project_index;
import :project.command;
import :project.project;
import :vfs.path;

namespace clice {

namespace types = clang::driver::types;

types::ID language_of(const CommandRef& command) {
    return types::lookupTypeForTypeSpecifier(command.language.value);
}

llvm::SmallVector<Candidate, 2> Project::candidates(Fid header, Fid host) {
    auto header_path = file_table.resolve(header);
    auto host_path = file_table.resolve(host);
    llvm::SmallVector<Candidate, 2> fitting;
    for(auto& candidate: candidates(host)) {
        auto command = tree_request(host, candidate).command;
        if(!commands.pch_output(command.command) &&
           compatible(header_path, host_path, language_of(command))) {
            fitting.push_back(candidate);
        }
    }
    return fitting;
}

llvm::SmallVector<Fid> Project::ranked_hosts(Fid header) {
    std::array generations = {dep_graph.generation(), commands_epoch, index.manifest_generation()};
    auto [it, inserted] = hosting.rankings.try_emplace(header);
    auto& ranking = it->second;
    if(!inserted && ranking.generations == generations) {
        return ranking.hosts;
    }
    auto& files = file_table;
    auto header_path = files.resolve(header);
    auto header_stem = path::stem(header_path);
    auto header_dir = path::parent(header_path);
    auto sources = source_order(header_path);

    // The lexical scan follows each header's includes under the first
    // command that reached it; a unit whose compile resolved them
    // otherwise names the header in its index entries.
    using Score =
        std::tuple<std::size_t, int, int, std::pair<std::size_t, std::size_t>, std::string>;
    llvm::SmallVector<std::pair<Score, Fid>> scored;
    llvm::DenseSet<Fid> seen;
    auto add = [&](Fid candidate) {
        // A header with an entry of its own contributes to itself.
        if(candidate == header || !seen.insert(candidate).second ||
           candidates(header, candidate).empty()) {
            return;
        }
        auto host_path = files.resolve(candidate);
        // A host compiled from a database the header's rules name comes
        // first; one living on a default command comes after every
        // database.
        std::size_t source_rank = sources.size();
        if(auto own = entries(candidate); !own.empty()) {
            source_rank = ranges::find(sources, own.front().source) - sources.begin();
        }
        int stem_match = path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = path::parent(host_path) == header_dir ? 0 : 1;
        scored.push_back({
            {source_rank,
             stem_match, same_dir,
             path::tree_distance(header_path, host_path),
             host_path.str()},
            candidate
        });
    };
    // A source another one includes still compiles on its own.
    for(auto candidate: dep_graph.find_readers(header, [&](Fid file) { return is_unit(file); })) {
        add(candidate);
    }
    index.for_each_contributor(header, add);
    ranges::sort(scored);

    ranking = {
        .hosts = ranges::to<llvm::SmallVector<Fid>>(views::values(scored)),
        .generations = generations,
    };
    return ranking.hosts;
}

namespace {

/// Where `host` under its own command `candidate` enters `header` at its
/// `occurrence`th place; none when it enters it fewer times.
std::optional<HostSearch> enter_at(Project& project,
                                   Fid header,
                                   Fid host,
                                   const Candidate& candidate,
                                   std::uint32_t occurrence) {
    auto request = project.tree_request(host, candidate);
    auto chains = project.include_chains(request.key, header);
    if(!chains) {
        return HostSearch{.missing = std::move(request)};
    }
    if(occurrence < chains->size()) {
        return HostSearch{.host = std::move((*chains)[occurrence]), .occurrence = occurrence};
    }
    return std::nullopt;
}

}  // namespace

HostSearch Project::find_chosen_host(Fid header, const ContextChoice& choice) {
    auto offered = candidates(header, choice.host);
    if(offered.empty()) {
        return {};
    }
    auto picked = chosen_candidate(offered, choice.base).value_or(offered.front());
    if(auto found = enter_at(*this, header, choice.host, picked, choice.occurrence.value_or(0))) {
        return std::move(*found);
    }
    return {};
}

HostSearch Project::find_ranked_host(Fid header) {
    // LIMITATION: a host with several commands (one per build configuration)
    // is tried under the first that fits alone: a header that only another of
    // its commands includes is hosted elsewhere, or not at all.
    for(auto host: ranked_hosts(header)) {
        if(auto found = enter_at(*this, header, host, candidates(header, host).front(), 0)) {
            return std::move(*found);
        }
    }
    return {};
}

Fid Project::host_of(Fid header) {
    auto search = find_ranked_host(header);
    if(search.missing) {
        return search.missing->key.first;
    }
    return search.host ? search.host->file : Fid{};
}

std::optional<std::uint32_t> Project::count_occurrences(Fid host, Fid header) {
    auto offered = candidates(header, host);
    if(offered.empty()) {
        return 0;
    }
    auto chains = include_chains(tree_request(host, offered.front()).key, header);
    if(!chains) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(chains->size());
}

}  // namespace clice
