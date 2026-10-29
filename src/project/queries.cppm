module;

#include "support/prelude.h"

/// Answers about the loaded build: its files, their include dependencies
/// and what a change to one reaches, and whether the index has caught up
/// with its units.

module clice:project.queries;

import :project.project;
import :vfs.path;

namespace clice {

struct FileInfo {
    std::string path;
    std::string kind;
    std::optional<std::string> module_name;
};

struct ProjectFilesResult {
    std::vector<FileInfo> files;
    int total = 0;
};

struct DepEntry {
    std::string path;
    int depth = 0;
};

struct FileDepsResult {
    std::string file;
    std::vector<DepEntry> includes;
    std::vector<DepEntry> includers;
};

struct ImpactAnalysisResult {
    std::vector<std::string> direct_dependents;
    std::vector<std::string> transitive_dependents;
    std::vector<std::string> affected_modules;
};

/// The build's files, `filter` one of all, source, header, module; the
/// error says why the question cannot be answered.
std::expected<ProjectFilesResult, std::string> project_files(Project& project,
                                                             llvm::StringRef filter);

/// The files reachable from `path` along include edges, `direction` one of
/// includes, includers, both; `depth` levels at most, 0 meaning unbounded.
std::expected<FileDepsResult, std::string>
    file_deps(Project& project, const Spelling& path, llvm::StringRef direction, int depth);

/// What a change to `path` reaches: its direct includers, the sources
/// hosting it, and the modules among them.
std::expected<ImpactAnalysisResult, std::string> impact_analysis(Project& project,
                                                                 const Spelling& path);

/// Whether some unit of the build the index is to hold has no record in
/// it yet (see feature::RenameScope::units_pending).
bool units_pending(Project& project);

}  // namespace clice
