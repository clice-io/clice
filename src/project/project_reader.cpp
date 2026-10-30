module clice;

import :index.database;
import :project.command_resolver;
import :project.hosting;
import :project.project_reader;

namespace clice {

std::string damaged_index_error(const Project& project) {
    return std::format(
        "the index database at {} is damaged (the log above says where); delete that "
        "directory and run `clice index` to rebuild it",
        index::database_directory(*project.store, project.database_key));
}

feature::query::Context ProjectReader::context() const {
    return {
        .query = query,
        .root = project.config.project_root,
        .cwd = Spelling::cwd(),
        .documentation = project.config.documentation,
    };
}

const clang::LangOptions& ProjectReader::dialect(Fid file) {
    return clice::dialect(project, file, nullptr, host_of(project, file));
}

std::vector<std::string> ProjectReader::stale(std::vector<std::string> listed) const {
    for(auto file: gate.withheld()) {
        listed.emplace_back(files.display(file));
    }
    ranges::sort(listed);
    auto duplicates = ranges::unique(listed);
    listed.erase(duplicates.begin(), duplicates.end());
    return listed;
}

}  // namespace clice
