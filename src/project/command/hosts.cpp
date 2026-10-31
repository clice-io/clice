module clice;

import :command.file_kind;
import :index.project_index;
import :project.hosting;
import :project.project;
import :vfs.path;

namespace clice {

namespace types = clang::driver::types;

types::ID language_of(const CommandRef& command) {
    return types::lookupTypeForTypeSpecifier(command.language.value);
}

/// Whether `type` is C++ or a C++ module unit: the two share flags, so
/// each lends to the other.
static bool cxx_or_module(types::ID type) {
    return type == types::TY_CXX || type == types::TY_CXXModule;
}

bool compatible(llvm::StringRef path, llvm::StringRef unit, types::ID language) {
    auto file = suffix_type(path);
    if(file == types::TY_INVALID) {
        return path::extension(path) != ".cuh" || types::isCuda(language) || types::isHIP(language);
    }
    if(file == types::TY_CHeader) {
        return true;
    }
    if(types::onlyPrecompileType(file) && types::isCXX(file)) {
        return types::isCXX(language);
    }
    return file == language || (cxx_or_module(file) && cxx_or_module(language)) ||
           (file == suffix_type(unit) && language == types::lookupCXXTypeForCType(file));
}

std::pair<std::size_t, std::size_t> tree_distance(llvm::StringRef path, llvm::StringRef other) {
    std::size_t shared = 0;
    auto n = std::min(path.size(), other.size());
    while(shared < n && path[shared] == other[shared]) {
        shared += 1;
    }
    auto separator = [](char c) {
        return path::is_separator(c);
    };
    while(shared > 0 && !separator(path[shared - 1])) {
        shared -= 1;
    }
    return {path.size() - shared,
            static_cast<std::size_t>(ranges::count_if(other.substr(shared), separator))};
}

llvm::SmallVector<Candidate, 2> Project::host_commands(Fid header, Fid host) {
    auto header_path = file_table.resolve(header);
    auto host_path = file_table.resolve(host);
    llvm::SmallVector<Candidate, 2> fitting;
    for(auto& candidate: build.commands(host)) {
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
    auto sources = build.source_order(header_path);

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
           host_commands(header, candidate).empty()) {
            return;
        }
        auto host_path = files.resolve(candidate);
        // A host compiled from a database the header's rules name comes
        // first; one living on a default command comes after every
        // database.
        std::size_t source_rank = sources.size();
        if(auto entries = build.entries(candidate); !entries.empty()) {
            source_rank = ranges::find(sources, entries.front().source) - sources.begin();
        }
        int stem_match = path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = path::parent(host_path) == header_dir ? 0 : 1;
        scored.push_back({
            {source_rank,
             stem_match, same_dir,
             tree_distance(header_path, host_path),
             host_path.str()},
            candidate
        });
    };
    // A source another one includes still compiles on its own.
    auto unit = [&](Fid file) {
        return build.unit(file);
    };
    for(auto candidate: dep_graph.find_readers(header, unit)) {
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
    auto host = choice.host_path_id;
    if(auto picked = chosen_host_command(header, host, choice.command_hash, choice.base_hash)) {
        if(auto found = enter_at(*this, header, host, *picked, choice.occurrence.value_or(0))) {
            return std::move(*found);
        }
    }
    return {};
}

HostSearch Project::find_ranked_host(Fid header) {
    for(auto host: ranked_hosts(header)) {
        if(auto found = enter_at(*this, header, host, host_commands(header, host).front(), 0)) {
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
    auto commands = host_commands(header, host);
    if(commands.empty()) {
        return 0;
    }
    auto chains = include_chains(tree_request(host, commands.front()).key, header);
    if(!chains) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(chains->size());
}

}  // namespace clice
