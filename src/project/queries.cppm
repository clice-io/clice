module;

/// Answers about the loaded build: its files, their include dependencies
/// and what a change to one reaches, and whether the index has caught up
/// with its units.

module clice:project.queries;

import :support.prelude;

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

}  // namespace clice
